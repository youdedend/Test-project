// SPDX-License-Identifier: GPL-2.0-or-later
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Popups.h>

#include <windows.h>
#include <fileapifromapp.h>
#include <gamingdeviceinformation.h>
#include <memoryapi.h>
#include <process.h>
#include <unknwn.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <io.h>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "dump_panel.h"
#include "ini_file.h"
#include "prepare_game.h"
#include "sfo_version.h"

extern "C" void bbgpu_flush_caches(void);

#ifndef BB_PREPARED_FORMAT
#define BB_PREPARED_FORMAT "1"
#endif

extern "C" {
int bbport_uwp_run_probe(int argc, char** argv);
FILE* bb_uwp_fopen(const char* path, const char* mode);
DWORD bb_uwp_attributes(const char* path);
HANDLE bb_uwp_find(const char* pattern, WIN32_FIND_DATAA* out);
BOOL bb_uwp_find_next(HANDLE handle, WIN32_FIND_DATAA* out);
void runtime_memory_start_paging(void);
void runtime_memory_report(void);
uint64_t runtime_memory_pinned_bytes(void);
void runtime_file_report(void);
void runtime_audio_report(void);
void runtime_pad_report(void);
void runtime_savedata_report(void);
void runtime_wait_report(void);
void runtime_sample_threads(void);
int runtime_entered_game(void);
int bb_uwp_dialog_key(uint32_t key, int down, int repeat);
void bb_uwp_overlay_configure(const char* base, const char* patch, const char* index_path);
uint64_t bbgpu_uwp_present_count(void);
int bbgpu_uwp_active_precompile_diag(uint32_t* out_idx, uint32_t* out_total, uint32_t* out_stage,
                                     uint64_t* out_hash, uint64_t* out_partner_hash);
}

static std::atomic<bool> g_finished{false};
static std::atomic<bool> g_game_mode{false};
static std::atomic<bool> g_window_closed{false};
static std::atomic<bool> g_app_visible{true};
static uint64_t g_memory_limit_override = 0;
static winrt::agile_ref<winrt::Windows::UI::Core::CoreWindow> g_core_window;
static winrt::Windows::Storage::StorageFolder g_dump_folder_lease{nullptr};

extern "C" void bb_uwp_pager_thread_init(void) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
}

extern "C" uint64_t bb_uwp_app_memory_usage(void) {
    return winrt::Windows::System::MemoryManager::AppMemoryUsage();
}

extern "C" uint64_t bb_uwp_app_memory_limit(void) {
    const uint64_t actual = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
    return g_memory_limit_override ? std::min(g_memory_limit_override, actual) : actual;
}

extern "C" int bb_uwp_app_visible(void) {
    return g_app_visible.load(std::memory_order_relaxed) ? 1 : 0;
}

extern "C" IUnknown* bb_uwp_core_window(void) {
    auto window = g_core_window.get();
    void* result = nullptr;
    winrt::copy_to_abi(window, result);
    return reinterpret_cast<IUnknown*>(result);
}

[[noreturn]] static void exit_now(const char* reason) {
    if (!g_finished.load()) {
        std::printf("UWP lifecycle: exiting (%s)\n", reason);
    }
    std::fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), 0);
    for (;;) {
        Sleep(INFINITE);
    }
}

[[noreturn]] static void close_app(const char* reason) {
    if (auto win = g_core_window.get()) {
        win.Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::High, [] {
            winrt::Windows::ApplicationModel::Core::CoreApplication::Exit();
        });
    }
    for (int wait = 0; wait < 12 && !g_window_closed.load(); ++wait) {
        Sleep(250);
    }
    exit_now(g_window_closed.load() ? "window closed" : reason);
}

extern "C" void bb_uwp_request_quit(void) {
    close_app("quit requested from in-game menu");
}

extern "C" void bb_uwp_request_restart(void) {
    std::puts("UWP lifecycle: requesting app restart to apply startup patches...");
    std::fflush(nullptr);
    if (auto win = g_core_window.get()) {
        auto done = std::make_shared<std::promise<void>>();
        auto fut = done->get_future();
        win.Dispatcher().RunAsync(
            winrt::Windows::UI::Core::CoreDispatcherPriority::High, [done] {
                try {
                    auto op = winrt::Windows::ApplicationModel::Core::CoreApplication::
                        RequestRestartAsync(L"");
                    op.Completed([done](auto const& async_op,
                                        winrt::Windows::Foundation::AsyncStatus status) {
                        if (status == winrt::Windows::Foundation::AsyncStatus::Completed) {
                            try {
                                const auto reason = async_op.GetResults();
                                std::printf("UWP lifecycle: RequestRestartAsync result=%d\n",
                                            static_cast<int>(reason));
                                std::fflush(stdout);
                            } catch (...) {
                            }
                        }
                        try {
                            done->set_value();
                        } catch (...) {
                        }
                    });
                } catch (...) {
                    try {
                        done->set_value();
                    } catch (...) {
                    }
                }
            });
        fut.wait_for(std::chrono::seconds(3));
    }
    close_app("restart fallback exit");
}

static void set_env_var(const char* name, const std::string& value) {
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableW(winrt::to_hstring(name).c_str(), winrt::to_hstring(value).c_str());
}

static void set_default_env_var(const char* name, const std::string& value) {
    if (const char* existing = std::getenv(name); existing && *existing) {
        return;
    }
    set_env_var(name, value);
}

/// `cancel_index` is the button the dialog reports when the player dismisses it (Back, B, escape).
/// It used to be the last one; a panel that appends a toggle after its last real choice has to say
/// which button that is, or dismissing the panel would flip the toggle instead of closing it.
static int ask_player(const std::string& text, const std::vector<std::string>& buttons,
                      int cancel_index = -1) {
    std::printf("Player dialog: %s\n", text.c_str());
    auto choice = std::make_shared<std::promise<int>>();
    auto answer = choice->get_future();
    if (cancel_index < 0 || cancel_index >= static_cast<int>(buttons.size())) {
        cancel_index = static_cast<int>(buttons.size()) - 1;
    }
    g_core_window.get().Dispatcher().RunAsync(
        winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
        [text, buttons, choice, cancel_index] {
            try {
                winrt::Windows::UI::Popups::MessageDialog dialog(winrt::to_hstring(text),
                                                                 L"Bloodborne");
                for (size_t i = 0; i < buttons.size(); ++i) {
                    dialog.Commands().Append(winrt::Windows::UI::Popups::UICommand(
                        winrt::to_hstring(buttons[i]), nullptr, winrt::box_value(int(i))));
                }
                dialog.DefaultCommandIndex(0);
                dialog.CancelCommandIndex(uint32_t(cancel_index));
                dialog.ShowAsync().Completed(
                    [choice](const auto& op, winrt::Windows::Foundation::AsyncStatus status) {
                        int index = -1;
                        try {
                            if (status == winrt::Windows::Foundation::AsyncStatus::Completed) {
                                if (auto cmd = op.GetResults()) {
                                    index = winrt::unbox_value<int>(cmd.Id());
                                }
                            }
                        } catch (const winrt::hresult_error&) {
                        }
                        choice->set_value(index);
                    });
            } catch (const winrt::hresult_error& err) {
                std::printf("Player dialog failed: 0x%08x\n", static_cast<unsigned>(err.code()));
                choice->set_value(-1);
            }
        });
    const int idx = answer.get();
    std::printf("Player choice: %d\n", idx);
    return idx;
}

static void stop_with_message(const std::string& message, bool player) {
    std::printf("STOP: %s\n", message.c_str());
    std::fflush(stdout);
    if (!player) {
        return;
    }
    ask_player(message, {"OK"});
    close_app("closed after error dialog");
}

static bool read_file_bytes(const std::string& path, BbPrepare::Bytes& data) {
    FILE* f = bb_uwp_fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    data.clear();
    std::vector<uint8_t> chunk(1 << 20);
    for (size_t n; (n = std::fread(chunk.data(), 1, chunk.size(), f)) > 0;) {
        data.insert(data.end(), chunk.begin(), chunk.begin() + n);
    }
    const bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}

static bool write_file_bytes(const std::string& path, const BbPrepare::Bytes& data) {
    FILE* f = bb_uwp_fopen(path.c_str(), "wb");
    if (!f) {
        return false;
    }
    const bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

static bool file_exists(const std::string& path) {
    const DWORD attr = bb_uwp_attributes(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool dir_exists(const std::string& path) {
    const DWORD attr = bb_uwp_attributes(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/// Removes a directory tree the app itself created (Prepared\overlay). Only LocalState paths are
/// ever passed in; nothing on the player's dump is touched.
static void DeleteTree(const std::string& path) {
    if (path.empty()) {
        return;
    }
    WIN32_FIND_DATAA entry{};
    const std::string pattern = path + (path.back() == '\\' ? "*" : "\\*");
    HANDLE h = bb_uwp_find(pattern.c_str(), &entry);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::string name = entry.cFileName;
            if (name == "." || name == "..") {
                continue;
            }
            const std::string child = path + "\\" + name;
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                DeleteTree(child);
            } else {
                DeleteFileFromAppW(winrt::to_hstring(child).c_str());
            }
        } while (bb_uwp_find_next(h, &entry));
        FindClose(h);
    }
    RemoveDirectoryFromAppW(winrt::to_hstring(path).c_str());
}

static bool dump_readable(const std::string& dump) {
    FILE* probe = bb_uwp_fopen((dump + "\\eboot.bin").c_str(), "rb");
    if (probe) {
        std::fclose(probe);
    }
    return probe != nullptr;
}

static bool is_bloodborne_dump(const std::string& folder) {
    BbPrepare::Bytes sfo;
    return dump_readable(folder) && read_file_bytes(folder + "\\sce_sys\\param.sfo", sfo) &&
           BbPrepare::CheckTitle(sfo).empty();
}

static std::string describe_place(const std::string& dump) {
    if (_strnicmp(dump.c_str(), "D:\\DevelopmentFiles", 19) == 0) {
        return "internal storage";
    }
    const auto local =
        winrt::to_string(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path());
    if (!local.empty() && _strnicmp(dump.c_str(), local.c_str(), local.size()) == 0) {
        return "app LocalState";
    }
    return "USB drive";
}

static std::wstring game_folder_file() {
    return std::wstring(
               winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path()) +
           L"\\game-folder.txt";
}

static void remember_dump(const std::string& dump) {
    std::ofstream f(game_folder_file(), std::ios::trunc);
    f << dump << "\n";
}

static std::string chosen_dump() {
    using namespace winrt::Windows::Storage::AccessCache;
    std::string broker_dump;
    try {
        auto list = StorageApplicationPermissions::FutureAccessList();
        if (list.ContainsItem(L"dump")) {
            /* Redeem and hold the StorageFolder token for process lifetime BEFORE probing
             * direct *FromAppW paths so RuntimeBroker grants full folder access on restarts. */
            g_dump_folder_lease = list.GetFolderAsync(L"dump").get();
            if (g_dump_folder_lease) {
                broker_dump = winrt::to_string(g_dump_folder_lease.Path());
            }
        }
    } catch (const winrt::hresult_error& err) {
        std::printf("Chosen game folder lookup: 0x%08x\n", static_cast<unsigned>(err.code()));
    }
    {
        std::ifstream f(game_folder_file());
        std::string dump;
        if (std::getline(f, dump) && !dump.empty()) {
            if (dump.back() == '\r') {
                dump.pop_back();
            }
            const bool readable = dump_readable(dump);
            std::printf("Chosen game folder %s: %s\n", dump.c_str(),
                        readable ? "found" : "not readable");
            if (readable) {
                return dump;
            }
        }
    }
    if (!broker_dump.empty()) {
        const bool readable = dump_readable(broker_dump);
        std::printf("Chosen game folder %s: %s\n", broker_dump.c_str(),
                    readable ? "found" : "not readable");
        if (readable) {
            return broker_dump;
        }
    }
    return {};
}

static winrt::Windows::Storage::StorageFolder pick_folder() {
    using winrt::Windows::Storage::StorageFolder;
    auto choice = std::make_shared<std::promise<StorageFolder>>();
    auto answer = choice->get_future();
    g_core_window.get().Dispatcher().RunAsync(
        winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [choice] {
            try {
                winrt::Windows::Storage::Pickers::FolderPicker picker;
                picker.SuggestedStartLocation(
                    winrt::Windows::Storage::Pickers::PickerLocationId::ComputerFolder);
                picker.FileTypeFilter().Append(L"*");
                picker.PickSingleFolderAsync().Completed(
                    [choice](const auto& op, winrt::Windows::Foundation::AsyncStatus status) {
                        StorageFolder folder{nullptr};
                        try {
                            if (status == winrt::Windows::Foundation::AsyncStatus::Completed) {
                                folder = op.GetResults();
                            }
                        } catch (const winrt::hresult_error&) {
                        }
                        choice->set_value(folder);
                    });
            } catch (const winrt::hresult_error& err) {
                std::printf("Folder picker failed: 0x%08x\n", static_cast<unsigned>(err.code()));
                choice->set_value(StorageFolder{nullptr});
            }
        });
    return answer.get();
}

static std::string use_picked(const winrt::Windows::Storage::StorageFolder& folder,
                              std::string& problem) {
    auto dump = winrt::to_string(folder.Path());
    bool listed = false;
    unsigned listed_error = 0;
    try {
        folder.GetFileAsync(L"eboot.bin").get();
        listed = true;
    } catch (const winrt::hresult_error& err) {
        listed_error = static_cast<unsigned>(err.code());
    }
    const bool readable = !dump.empty() && dump_readable(dump);
    const DWORD read_error = readable ? 0 : GetLastError();
    std::printf("Folder picker: '%s'; eboot.bin via broker: %s (0x%08x), via FromApp: %s (%lu)\n",
                dump.c_str(), listed ? "found" : "not found", listed_error,
                readable ? "readable" : "not readable", read_error);
    if (readable) {
        BbPrepare::Bytes sfo;
        if (read_file_bytes(dump + "\\sce_sys\\param.sfo", sfo)) {
            if (auto err = BbPrepare::CheckTitle(sfo); !err.empty()) {
                problem = "The dump in " + dump + " " + err;
                return {};
            }
        }
        g_dump_folder_lease = folder;
        winrt::Windows::Storage::AccessCache::StorageApplicationPermissions::FutureAccessList()
            .AddOrReplace(L"dump", folder);
        remember_dump(dump);
        return dump;
    }
    problem = listed
                  ? "The game cannot read its files directly from " + dump +
                        ".\n\nCopy the dump to a USB drive (e.g. E:\\dumps\\Bloodborne) or "
                        "D:\\DevelopmentFiles\\Bloodborne and choose it there."
                  : "There is no eboot.bin in " +
                        (dump.empty() ? std::string("this folder") : dump) +
                        ".\n\nChoose the root folder of your decrypted PS4 Bloodborne dump: "
                        "the folder containing eboot.bin, sce_module, sce_sys, and dvdroot_ps4.";
    return {};
}

static std::string choose_dump_loop(std::string prompt) {
    for (;;) {
        if (ask_player(prompt, {"Choose folder", "Close"}) != 0) {
            return {};
        }
        auto folder = pick_folder();
        if (!folder) {
            std::puts("Folder picker: cancelled");
            continue;
        }
        if (auto dump = use_picked(folder, prompt); !dump.empty()) {
            return dump;
        }
    }
}

static std::string switch_game_folder(const std::string& dump) {
    remember_dump(dump);
    const char* current = std::getenv("BB_UWP_DUMP");
    if (current && _stricmp(dump.c_str(), current) == 0) {
        return "This is already the active Bloodborne folder.";
    }
    std::printf("Game folder switched to: %s\n", dump.c_str());
    ask_player("Game folder set to:\n" + dump + " (" + describe_place(dump) +
                   ")\n\nStart Bloodborne again to play from this folder.",
               {"OK"});
    close_app("game folder changed");
}

extern "C" void bb_uwp_choose_game_folder(char* message, size_t size) {
    auto folder = pick_folder();
    std::string text;
    if (!folder) {
        std::puts("Folder picker: cancelled");
    } else if (auto dump = use_picked(folder, text); !dump.empty()) {
        text = switch_game_folder(dump);
    }
    std::snprintf(message, size, "%s", text.c_str());
}

extern "C" void bb_uwp_use_game_folder(const char* dump, char* message, size_t size) {
    if (!dump || !*dump) {
        if (message && size) message[0] = '\0';
        return;
    }
    std::snprintf(message, size, "%s", switch_game_folder(dump).c_str());
}

extern "C" void bb_uwp_game_folder_line(char* out, size_t size) {
    const char* current = std::getenv("BB_UWP_DUMP");
    if (!current || !*current) {
        std::snprintf(out, size, "%s", "");
        return;
    }
    std::snprintf(out, size, "Game folder: %s (%s)", current, describe_place(current).c_str());
}

static std::vector<std::string> scan_dumps_bfs(const std::string& root, int max_depth,
                                               size_t limit = 8) {
    std::vector<std::pair<std::string, int>> queue = {{root, 0}};
    std::vector<std::string> found;
    for (size_t idx = 0; idx < queue.size() && idx < 400 && found.size() < limit; ++idx) {
        auto [folder, depth] = queue[idx];
        if (is_bloodborne_dump(folder)) {
            found.push_back(folder);
            continue;
        }
        if (depth >= max_depth) {
            continue;
        }
        WIN32_FIND_DATAA entry{};
        const std::string pattern =
            folder + (folder.back() == '\\' ? "*" : "\\*");
        HANDLE h = bb_uwp_find(pattern.c_str(), &entry);
        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }
        do {
            const std::string name = entry.cFileName;
            if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name == "." ||
                name == ".." || name == "WindowsApps" ||
                name == "System Volume Information" || name == "$RECYCLE.BIN") {
                continue;
            }
            queue.emplace_back(folder + (folder.back() == '\\' ? "" : "\\") + name, depth + 1);
        } while (bb_uwp_find_next(h, &entry));
        FindClose(h);
    }
    return found;
}

static std::vector<std::string> usb_dumps() {
    std::vector<std::string> roots;
    try {
        for (const auto& drive :
             winrt::Windows::Storage::KnownFolders::RemovableDevices().GetFoldersAsync().get()) {
            auto root = winrt::to_string(drive.Path());
            if (!root.empty()) {
                roots.push_back(root);
            }
        }
    } catch (const winrt::hresult_error& err) {
        std::printf("USB drive enumeration: 0x%08x\n", static_cast<unsigned>(err.code()));
    }
    for (const char* fallback : {"E:\\", "F:\\"}) {
        if (std::none_of(roots.begin(), roots.end(), [&](const std::string& r) {
                return _strnicmp(r.c_str(), fallback, 2) == 0;
            })) {
            roots.push_back(fallback);
        }
    }

    std::vector<std::string> found;
    auto add_unique = [&](const std::string& p) {
        if (!p.empty() && std::none_of(found.begin(), found.end(), [&](const std::string& k) {
                return _stricmp(k.c_str(), p.c_str()) == 0;
            })) {
            found.push_back(p);
        }
    };

    for (auto root : roots) {
        if (root.back() != '\\') {
            root += '\\';
        }
        for (const char* quick : {"dumps\\Bloodborne", "Bloodborne", "Games\\Bloodborne"}) {
            const std::string candidate = root + quick;
            if (is_bloodborne_dump(candidate)) {
                std::printf("USB dump found: %s\n", candidate.c_str());
                add_unique(candidate);
            }
        }
        for (const auto& scanned : scan_dumps_bfs(root, 2, 4)) {
            add_unique(scanned);
        }
    }
    return found;
}

static std::vector<std::string> localstate_dumps() {
    const std::string local =
        winrt::to_string(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path());
    std::vector<std::string> found;
    for (const char* sub : {"game", "dump", "Bloodborne"}) {
        const std::string candidate = local + "\\" + sub;
        if (is_bloodborne_dump(candidate)) {
            found.push_back(candidate);
        }
    }
    return found;
}

static std::vector<std::string> internal_dumps(const std::string& root = "D:\\DevelopmentFiles") {
    const auto t0 = std::chrono::steady_clock::now();
    auto found = scan_dumps_bfs(root, 3, 8);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    std::printf("Internal dump search (%s): %zu found in %lld ms\n", root.c_str(), found.size(),
                static_cast<long long>(ms));
    return found;
}

extern "C" void bb_uwp_find_game_folders(char* out, size_t size) {
    std::vector<std::string> found;
    auto add = [&](const std::string& dump) {
        if (dump.empty() ||
            std::any_of(found.begin(), found.end(), [&](const std::string& k) {
                return _stricmp(k.c_str(), dump.c_str()) == 0;
            })) {
            return;
        }
        found.push_back(dump);
    };
    if (const char* current = std::getenv("BB_UWP_DUMP")) {
        add(current);
    }
    add(chosen_dump());
    for (const auto& d : usb_dumps()) add(d);
    for (const auto& d : internal_dumps()) add(d);
    for (const auto& d : localstate_dumps()) add(d);

    std::string text;
    for (const auto& d : found) {
        text += d + "\t" + describe_place(d) + "\n";
    }
    std::snprintf(out, size, "%s", text.c_str());
}

static void ensure_default_ini(const std::string& ini_path) {
    if (file_exists(ini_path)) {
        return;
    }
    const char* defaults =
        "# Bloodborne Xbox Series X|S UWP settings (in-game overlay: L3 + R3)\n"
        "# skip_intro=1 skips the intro logos and movie (also in the in-game menu); absent = asked\n"
        "# once on the first start.\n"
        "upscaler=off\n"
        "scene_res=1280x720\n"
        "output_res=1920x1080\n"
        "vsync=1\n"
        "fps_cap=60\n"
        "gamma=1.00\n"
        "fps_patch=60\n"
        "show_fps=1\n"
        "mute_unfocused=0\n"
        "button_prompts=xbox\n"
        "effect_chromatic_aberration=0\n"
        "effect_dof=1\n"
        "effect_motion_blur=0\n"
        "effect_ssao=1\n"
        "effect_game_aa=1\n"
        "effect_dynamic_shadows=1\n"
        "effect_ssr=0\n"
        "model_lod=0\n"
        "\n"
        "[dump]\n"
        "# Empty patch_path auto-detects the update folder next to the game folder.\n"
        "patch_path=\n"
        "use_patch=1\n";
    BbPrepare::Bytes bytes(defaults, defaults + std::strlen(defaults));
    write_file_bytes(ini_path, bytes);
}

/// Parses `key=value` lines (see xbox/platform/ini_file.h). `[dump] patch_path=` becomes
/// `dump.patch_path`; the bare keys the patch settings use keep resolving as before.
static std::map<std::string, std::string> parse_kv_file(const std::string& path) {
    BbPrepare::Bytes raw;
    if (!read_file_bytes(path, raw)) {
        return {};
    }
    return BbIni::Parse(std::string(raw.begin(), raw.end()));
}

/// Writes one key of one section, keeping every other line of the player's settings file intact.
static bool set_ini_value(const std::string& path, const std::string& section, const std::string& key,
                          const std::string& value) {
    BbPrepare::Bytes raw;
    std::string text;
    if (read_file_bytes(path, raw)) {
        text.assign(raw.begin(), raw.end());
    }
    bool changed = false;
    const std::string updated = BbIni::SetValue(text, section, key, value, &changed);
    if (!changed) {
        return true;
    }
    std::printf("Settings %s: %s.%s=%s\n", path.c_str(), section.c_str(), key.c_str(), value.c_str());
    if (!write_file_bytes(path, BbPrepare::Bytes(updated.begin(), updated.end()))) {
        std::printf("Settings %s: write failed\n", path.c_str());
        return false;
    }
    return true;
}

/// Writes one bare (top-level) key of the settings file, the layout the patch settings use
/// (`skip_intro=1`, `effect_ssao=0`), keeping every line of the [dump] section intact.
static bool set_ini_top_level(const std::string& path, const std::string& key,
                              const std::string& value) {
    BbPrepare::Bytes raw;
    std::string text;
    if (read_file_bytes(path, raw)) {
        text.assign(raw.begin(), raw.end());
    }
    bool changed = false;
    const std::string updated = BbIni::SetTopLevel(text, key, value, &changed);
    if (!changed) {
        return true;
    }
    std::printf("Settings %s: %s=%s\n", path.c_str(), key.c_str(), value.c_str());
    if (!write_file_bytes(path, BbPrepare::Bytes(updated.begin(), updated.end()))) {
        std::printf("Settings %s: write failed\n", path.c_str());
        return false;
    }
    return true;
}

static std::string resolve_render_res_override(const std::map<std::string, std::string>&) {
    if (const char* forced = std::getenv("BB_UWP_RENDER_RES"); forced && *forced) {
        return forced;
    }
    return {};
}

// ---- dump layout: launcher panel, patch folder picker, [dump] persistence --------------------

static winrt::Windows::Storage::StorageFolder g_patch_folder_lease{nullptr};

/// What the launcher decided about the player's dump, and therefore what the game will run.
struct DumpSelection {
    BbPrepare::DumpLayout layout;
    bool use_patch = true;            ///< `[dump] use_patch` in bbport.ini
    bool unpatched_confirmed = false; ///< the player accepted "Continue anyway (unpatched)"

    [[nodiscard]] bool IsMerged() const {
        return layout.kind == BbPrepare::DumpLayout::Kind::Merged;
    }
    /// True when the update folder is found, enabled and actually decided the version.
    [[nodiscard]] bool Patched() const {
        return use_patch && layout.kind == BbPrepare::DumpLayout::Kind::Split;
    }
    /// Folder that supplies eboot.bin/sce_module/sce_sys/dvdroot_ps4.
    [[nodiscard]] const std::string& BasePath() const { return layout.base_path; }
    /// Version the game will actually report, always read from a param.sfo.
    [[nodiscard]] std::string EffectiveVersion() const {
        if (Patched()) return layout.patch.Version();
        return layout.base.Version();
    }
    /// True when the packaged red-zone patch set can be applied to what will run.
    [[nodiscard]] bool CanRunPatched() const {
        return BbPrepare::IsPatchTargetVersion(EffectiveVersion());
    }
};

/// What the packaged patch set can do to what will actually run.
enum class PatchState {
    Required, ///< the effective build is the one the patch set was reverse engineered for
    Optional, ///< a newer build: only the in-image fingerprint check can decide, patches may be empty
    Skipped,  ///< base game only (or the update turned off): red-zone patches must not run
};

static const char* PatchStateName(PatchState state) {
    switch (state) {
    case PatchState::Required:
        return "required";
    case PatchState::Optional:
        return "optional";
    default:
        return "skipped";
    }
}

/// The red-zone patch set is only valid for the exact build it was reverse engineered for
/// (patches/redzone.json). Anything else must not get the trampolines, whatever the dump layout.
static PatchState EffectivePatchState(const DumpSelection& selection) {
    const std::string effective = selection.EffectiveVersion();
    if (BbPrepare::IsPatchTargetVersion(effective)) {
        return PatchState::Required;
    }
    if (BbPrepare::CompareVersions(effective, BbPrepare::kPatchTargetVersion) > 0) {
        return PatchState::Optional;
    }
    return PatchState::Skipped;
}

/// Redeems the StorageFolder token of a previously picked update folder so RuntimeBroker grants
/// direct *FromAppW access to it on this launch, exactly like the game folder token.
static void redeem_patch_lease() {
    using namespace winrt::Windows::Storage::AccessCache;
    try {
        auto list = StorageApplicationPermissions::FutureAccessList();
        if (list.ContainsItem(L"patch")) {
            g_patch_folder_lease = list.GetFolderAsync(L"patch").get();
            if (g_patch_folder_lease) {
                std::printf("Patch folder lease: %s\n",
                            winrt::to_string(g_patch_folder_lease.Path()).c_str());
            }
        }
    } catch (const winrt::hresult_error& err) {
        std::printf("Patch folder lease lookup: 0x%08x\n", static_cast<unsigned>(err.code()));
    }
}

/// Validates a folder the player picked as the update, remembers it for the FutureAccessList and
/// returns its path. `problem` is shown to the player when the folder cannot be used.
static std::string use_picked_patch(const winrt::Windows::Storage::StorageFolder& folder,
                                    const BbPrepare::SfoInfo& base, std::string& problem) {
    const std::string path = winrt::to_string(folder.Path());
    const auto info = BbPrepare::ReadParamSfo(path);
    std::printf("Patch folder picker: '%s'; param.sfo: %s\n", path.c_str(),
                info ? "read" : "missing or unreadable");
    if (path.empty()) {
        problem = "That folder cannot be read by the app.\n\nCopy the update folder to the USB "
                  "drive next to the game folder and pick it there.";
        return {};
    }
    if (!info) {
        problem = "There is no readable sce_sys\\param.sfo in:\n" + path +
                  "\n\nChoose the folder holding the Bloodborne update: it contains eboot.bin, "
                  "sce_module, sce_sys\\param.sfo and dvdroot_ps4. Decrypted dumps usually keep it "
                  "next to the game folder, for example ...\\Bloodborne-patch.";
        return {};
    }
    if (!base.title_id.empty() && !info->title_id.empty() &&
        _stricmp(base.title_id.c_str(), info->title_id.c_str()) != 0) {
        problem = "That folder belongs to a different game (TITLE_ID " + info->title_id +
                  ", this dump is " + base.title_id + ").";
        return {};
    }
    if (BbPrepare::CompareVersions(info->Version(), base.Version()) <= 0) {
        problem = "The folder:\n" + path + "\n\nreports app_ver=" + info->Version() +
                  " (APP_TYPE=" + info->app_type + "), which is not newer than the game dump (" +
                  base.Version() + ").\n\nPick the " +
                  std::string(BbPrepare::kPatchTargetVersion) +
                  " update folder, not the base game and not an older patch.";
        return {};
    }
    if (!file_exists(path + "\\eboot.bin")) {
        problem = "The update folder " + path +
                  " has a param.sfo but no eboot.bin, so the patched executable is missing.";
        return {};
    }
    try {
        g_patch_folder_lease = folder;
        winrt::Windows::Storage::AccessCache::StorageApplicationPermissions::FutureAccessList()
            .AddOrReplace(L"patch", folder);
    } catch (const winrt::hresult_error& err) {
        std::printf("Patch folder lease: 0x%08x\n", static_cast<unsigned>(err.code()));
    }
    return path;
}

/// The panel text builders live in xbox/platform/dump_panel.h so the CI test can pin the exact
/// sentences the player sees (main.cpp itself needs WinRT and cannot be built on the runner).
static BbPanel::Panel PanelOf(const DumpSelection& sel) {
    BbPanel::Panel panel;
    panel.base_path = sel.layout.base_path;
    panel.base = sel.layout.base;
    panel.patch_path = sel.layout.patch_path;
    panel.patch = sel.layout.patch;
    panel.use_patch = sel.use_patch;
    panel.effective_ver = sel.EffectiveVersion();
    panel.can_run_patched = sel.CanRunPatched();
    return panel;
}

static std::string describe_dump(const std::string& path, const BbPrepare::SfoInfo& info) {
    return BbPanel::DescribeDump(path, info);
}

static std::string dump_panel_text(const DumpSelection& sel) {
    return BbPanel::PanelText(PanelOf(sel));
}

static std::string base_only_banner(const DumpSelection& sel) {
    return BbPanel::BaseOnlyBanner(PanelOf(sel));
}

/// Asks once, before the first start that applies patches, whether the intro should be skipped.
///
/// The intro (FromSoftware/Sony logos and the title movie) is skipped by the "Skip Intro" patch in
/// patches/Bloodborne.xml, which BuildPatches() compiles at start - the same `skip_intro` key the
/// Windows launcher's settings write (scripts/patches.py). The in-game menu can change it later, but
/// only for the next start, which is exactly what the intro is not: reachable. A MessageDialog takes
/// three commands at most, so it cannot be a fourth button on the launcher panel.
///
/// "Answered" is the presence of the key: the default bbport.ini does not contain it, so a fresh
/// install is asked once and never again.
static void ask_intro_once(const std::string& ini_path) {
    auto ini = parse_kv_file(ini_path);
    if (ini.find("skip_intro") != ini.end()) {
        return;
    }
    const int choice =
        ask_player("Skip the intro?\n\nBloodborne can start straight at the title screen instead "
                   "of playing the FromSoftware/Sony logos and the title movie.\n\nYou can change "
                   "this later in the in-game menu (hold L3 + R3, \"Visual & gameplay patches\").",
                   {"Play the intro", "Skip the intro"}, /*cancel_index=*/0);
    set_ini_top_level(ini_path, "skip_intro", choice == 1 ? "1" : "0");
    std::printf("Launcher panel: intro %s%s\n", choice == 1 ? "skipped" : "played",
                choice < 0 ? " (dialog dismissed)" : "");
}

/// Shows the detected layout and applies the player's choice. Returns false when the app must not
/// start the game (unreadable dump, or the player closed the panel).
static bool launcher_select_dump_layout(const std::string& picked, const std::string& ini_path,
                                        bool player, DumpSelection& sel) {
    redeem_patch_lease();
    auto ini = parse_kv_file(ini_path);
    std::string saved_patch = [&ini] {
        const auto it = ini.find("dump.patch_path");
        return it == ini.end() ? std::string() : it->second;
    }();
    bool use_patch = BbIni::Flag(ini, "dump.use_patch", true);
    if (!saved_patch.empty() && !dir_exists(saved_patch)) {
        std::printf("Saved patch folder is gone: %s\n", saved_patch.c_str());
        saved_patch.clear();
        set_ini_value(ini_path, "dump", "patch_path", "");
    }

    // Before the panel, so the panel's "Intro: ..." line shows the answer just given. Never in a
    // headless start (run.txt mode, the memory self-test): there is no one to answer it.
    if (player) {
        ask_intro_once(ini_path);
        ini = parse_kv_file(ini_path);
    }

    for (;;) {
        sel.layout = BbPrepare::DetectDumpLayout(picked, saved_patch);
        sel.use_patch = use_patch;
        if (!sel.layout.IsValid()) {
            stop_with_message("The Bloodborne dump in " + picked +
                                  " has no readable sce_sys\\param.sfo.\n\nChoose the folder "
                                  "containing eboot.bin, sce_module, sce_sys, and dvdroot_ps4.",
                              player);
            return false;
        }
        std::printf("Launcher dump layout:\n%s\n", dump_panel_text(sel).c_str());
        std::fflush(stdout);
        if (!player) {
            std::printf("Headless launch (run.txt): no layout confirmation dialog; patches=%s\n",
                        sel.CanRunPatched() ? "applied" : "skipped");
            std::fflush(stdout);
            return true;
        }

        const std::string header = "Bloodborne dump\n\n";
        enum class Action { Launch, LaunchUnpatched, EnablePatch, PickPatch, ChangeFolder, Close };
        Action action = Action::Close;
        // bbport: the intro is skipped by a startup patch, so its state belongs in the panel: the
        // switch itself is in the in-game menu (Visual & gameplay patches), and a fresh install was
        // asked once at the top of this function.
        const std::string intro_line =
            BbIni::Flag(ini, "skip_intro", false)
                ? "\n\nIntro: skipped (logos and title movie). Change it in the in-game menu."
                : "\n\nIntro: played. Change it in the in-game menu.";
        if (sel.IsMerged()) {
            std::printf("Launcher panel: merged dump (effective=%s)\n",
                        sel.EffectiveVersion().c_str());
            const int choice = ask_player(header + dump_panel_text(sel) + "\n\nLaunch the game?" +
                                              intro_line,
                                          {"Launch", "Change game folder"});
            action = choice == 0 ? Action::Launch
                                 : (choice == 1 ? Action::ChangeFolder : Action::Close);
        } else if (!sel.layout.patch_path.empty() && sel.use_patch) {
            std::printf("Launcher panel: split dump (effective=%s)\n",
                        sel.EffectiveVersion().c_str());
            const int choice = ask_player(
                header + dump_panel_text(sel) +
                    "\n\nThe update is merged over the game dump inside the app's LocalState; "
                    "your USB folder is never modified." +
                    intro_line,
                {"Launch", "Run base only", "Pick patch folder"});
            action = choice == 0   ? Action::Launch
                     : choice == 1 ? Action::LaunchUnpatched
                     : choice == 2 ? Action::PickPatch
                                   : Action::Close;
        } else if (!sel.layout.patch_path.empty()) {
            std::printf("Launcher panel: split dump, patch disabled (effective=%s)\n",
                        sel.EffectiveVersion().c_str());
            const int choice = ask_player(
                header + dump_panel_text(sel) +
                    "\n\nThe update was found but \"Use patch\" is off: the game runs the "
                    "unpatched 30 FPS build." +
                    intro_line,
                {"Use patch", "Launch unpatched", "Pick patch folder"});
            action = choice == 0   ? Action::EnablePatch
                     : choice == 1 ? Action::LaunchUnpatched
                     : choice == 2 ? Action::PickPatch
                                   : Action::Close;
        } else {
            std::printf("Launcher panel: base-only dump (effective=%s)\n",
                        sel.EffectiveVersion().c_str());
            const int choice = ask_player(header + dump_panel_text(sel) + "\n\n" +
                                              base_only_banner(sel) + intro_line,
                                          {"Pick patch folder", "Continue anyway (unpatched)", "Close"});
            action = choice == 0   ? Action::PickPatch
                     : choice == 1 ? Action::LaunchUnpatched
                                   : Action::Close;
        }

        switch (action) {
        case Action::Launch:
            set_ini_value(ini_path, "dump", "use_patch", "1");
            return true;
        case Action::LaunchUnpatched:
            sel.use_patch = false;
            sel.unpatched_confirmed = true;
            set_ini_value(ini_path, "dump", "use_patch", "0");
            return true;
        case Action::EnablePatch:
            use_patch = true;
            set_ini_value(ini_path, "dump", "use_patch", "1");
            continue;
        case Action::ChangeFolder: {
            auto folder = pick_folder();
            if (!folder) {
                std::puts("Folder picker: cancelled");
                continue;
            }
            std::string problem;
            if (auto next = use_picked(folder, problem); !next.empty()) {
                switch_game_folder(next);
            } else {
                ask_player(problem, {"OK"});
            }
            continue;
        }
        case Action::PickPatch: {
            auto folder = pick_folder();
            if (!folder) {
                std::puts("Patch folder picker: cancelled");
                continue;
            }
            std::string problem;
            std::string patch_path = use_picked_patch(folder, sel.layout.base, problem);
            if (patch_path.empty()) {
                ask_player(problem, {"OK"});
                continue;
            }
            saved_patch = patch_path;
            use_patch = true;
            set_ini_value(ini_path, "dump", "patch_path", patch_path);
            set_ini_value(ini_path, "dump", "use_patch", "1");
            std::printf("Patch folder saved: %s\n", patch_path.c_str());
            std::fflush(stdout);
            continue;
        }
        case Action::Close:
        default:
            return false;
        }
    }
}

/// Reads `"key": "value"` out of a JSON document the app wrote itself (overlay.json).
static std::string JsonField(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const size_t at = json.find(needle);
    if (at == std::string::npos) {
        return {};
    }
    const size_t colon = json.find(':', at + needle.size());
    if (colon == std::string::npos) {
        return {};
    }
    const size_t open = json.find('"', colon + 1);
    if (open == std::string::npos) {
        return {};
    }
    const size_t close = json.find('"', open + 1);
    if (close == std::string::npos) {
        return {};
    }
    return json.substr(open + 1, close - open - 1);
}

/// Drops the prepared image and the driver pipeline cache together. Both describe the same guest
/// build, so when the overlay changes (a different update, or split <-> merged) the pair must be
/// invalidated at once instead of leaving a pipeline cache compiled for the other build behind.
static void invalidate_prepared(const std::string& prepared, const std::string& user_dir) {
    for (const char* name :
         {"boot-linked.bin", "content.bin", "eboot.elf", "app_ver.txt", "format.txt"}) {
        DeleteFileFromAppW(winrt::to_hstring(prepared + "\\" + name).c_str());
    }
    const std::string cache_root = user_dir + "\\cache";
    WIN32_FIND_DATAA entry{};
    HANDLE h = bb_uwp_find((cache_root + "\\*").c_str(), &entry);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        const std::string name = entry.cFileName;
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name == "." || name == "..") {
            continue;
        }
        const std::string driver_cache = cache_root + "\\" + name + "\\driver.vkcache";
        if (file_exists(driver_cache)) {
            std::printf("Prepare: dropping driver.vkcache (%s)\n", driver_cache.c_str());
            DeleteFileFromAppW(winrt::to_hstring(driver_cache).c_str());
        }
    } while (bb_uwp_find_next(h, &entry));
    FindClose(h);
}

static bool prepare_game_and_patches(const std::string& dump, const std::string& prepared,
                                     const std::string& ini_path, bool player, bool force_rebuild,
                                     const DumpSelection& selection) {
    const std::string pkg_path = winrt::to_string(
        winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path());

    // A separately dumped update is laid over the game dump inside LocalState: the boot container is
    // materialized into Prepared\overlay and every other path the update provides is redirected to
    // it by the platform file layer, so neither folder is ever copied wholesale or written to.
    const std::string overlay_manifest_path = prepared + "\\overlay.json";
    BbPrepare::Bytes previous_manifest;
    const std::string previous_digest =
        read_file_bytes(overlay_manifest_path, previous_manifest)
            ? JsonField(std::string(previous_manifest.begin(), previous_manifest.end()), "digest")
            : std::string();

    std::string src_root = dump;
    BbPrepare::OverlayImage overlay;
    if (selection.Patched()) {
        overlay = BbPrepare::BuildOverlayedImage(dump, selection.layout.patch_path,
                                                 prepared + "\\overlay");
        if (!overlay.IsValid()) {
            stop_with_message("The update folder " + selection.layout.patch_path +
                                  " could not be merged over the game dump.\n\nRe-copy the "
                                  "update folder and pick it again, or launch the base game.",
                              player);
            return false;
        }
        bb_uwp_overlay_configure(dump.c_str(), selection.layout.patch_path.c_str(),
                                 overlay.index_path.c_str());
        src_root = overlay.root;
    } else {
        bb_uwp_overlay_configure("", "", "");
        // A stale overlay from an earlier split launch must not be read by the prepared-image cache.
        DeleteTree(prepared + "\\overlay");
        DeleteFileFromAppW(winrt::to_hstring(overlay_manifest_path).c_str());
    }

    // Prepared\boot-linked.bin and <user>\cache\<serial>\driver.vkcache describe the same guest
    // build: when the overlay changes, both are stale together.
    const std::string overlay_digest = overlay.IsValid() ? overlay.digest : std::string();
    if (previous_digest != overlay_digest && !(previous_digest.empty() && overlay_digest.empty())) {
        const char* user_env = std::getenv("BB_USER_DIR");
        std::printf("%s\n", BbPanel::OverlayChangedLine(previous_digest, overlay_digest).c_str());
        invalidate_prepared(prepared, user_env ? user_env : "");
    }

    // One line per launch says exactly what is being prepared and which patches can apply to it.
    const PatchState patch_state = EffectivePatchState(selection);
    const std::string base_ver = selection.layout.base.Version();
    const std::string patch_ver = selection.layout.patch_path.empty()
                                      ? std::string("-")
                                      : selection.layout.patch.Version();
    const std::string effective_ver = selection.EffectiveVersion();
    std::printf("%s\n", BbPanel::PrepareLine(base_ver, patch_ver, effective_ver,
                                              PatchStateName(patch_state))
                            .c_str());
    if (patch_state == PatchState::Skipped) {
        std::printf("%s\n", BbPanel::SkippedPatchesLine(selection.unpatched_confirmed).c_str());
    }
    std::fflush(stdout);

    BbPrepare::Bytes redzone_bytes;
    if (!read_file_bytes(pkg_path + "\\patches\\redzone.json", redzone_bytes)) {
        stop_with_message("Packaged patches\\redzone.json is missing. Reinstall the app.", player);
        return false;
    }
    const std::string redzone_json(redzone_bytes.begin(), redzone_bytes.end());

    BbPrepare::Bytes xml_bytes;
    if (!read_file_bytes(pkg_path + "\\patches\\Bloodborne.xml", xml_bytes)) {
        stop_with_message("Packaged patches\\Bloodborne.xml is missing. Reinstall the app.", player);
        return false;
    }
    const std::string patch_xml(xml_bytes.begin(), xml_bytes.end());

    BbPrepare::Dump files;
    const std::pair<const char*, BbPrepare::Bytes*> required[] = {
        {"eboot.bin", &files.eboot},
        {"sce_module\\libc.prx", &files.libc},
        {"sce_module\\libSceFios2.prx", &files.fios},
        {"sce_sys\\param.sfo", &files.sfo},
    };
    for (auto [rel, dest] : required) {
        if (!read_file_bytes(src_root + "\\" + rel, *dest) || dest->empty()) {
            stop_with_message("The Bloodborne dump in " + src_root + " is incomplete:\n" + rel +
                                  " is missing or unreadable.\n\nMake sure eboot.bin, "
                                  "sce_module\\libc.prx, sce_module\\libSceFios2.prx, "
                                  "sce_sys\\param.sfo, and dvdroot_ps4 are present.",
                              player);
            return false;
        }
    }

    if (auto problem = BbPrepare::CheckTitle(files.sfo); !problem.empty()) {
        stop_with_message("The dump in " + dump + " " + problem, player);
        return false;
    }

    char marker[128];
    std::snprintf(marker, sizeof(marker), "bloodborne-uwp-prepared %s\nsource %016llx\n",
                  BB_PREPARED_FORMAT,
                  static_cast<unsigned long long>(BbPrepare::Fingerprint(files, redzone_json)));

    const std::string boot_path = prepared + "\\boot-linked.bin";
    const std::string content_path = prepared + "\\content.bin";
    const std::string eboot_elf_path = prepared + "\\eboot.elf";
    const std::string appver_path = prepared + "\\app_ver.txt";
    const std::string format_path = prepared + "\\format.txt";

    BbPrepare::Bytes eboot_elf;
    std::string app_ver;
    bool cached_ok = false;
    if (!force_rebuild) {
        BbPrepare::Bytes cur_marker, ver_bytes;
        if (read_file_bytes(format_path, cur_marker) &&
            std::string(cur_marker.begin(), cur_marker.end()) == marker &&
            file_exists(boot_path) && file_exists(content_path) &&
            read_file_bytes(eboot_elf_path, eboot_elf) && !eboot_elf.empty() &&
            read_file_bytes(appver_path, ver_bytes)) {
            app_ver.assign(ver_bytes.begin(), ver_bytes.end());
            while (!app_ver.empty() && (app_ver.back() == '\n' || app_ver.back() == '\r')) {
                app_ver.pop_back();
            }
            cached_ok = true;
            std::printf("Prepared boot image cached: %s (app_ver=%s)\n", prepared.c_str(),
                        app_ver.c_str());
        }
    }

    if (!cached_ok) {
        const bool upgrading_cache = file_exists(boot_path);
        if (player && !upgrading_cache &&
            ask_player("Bloodborne found:\n" + dump +
                           "\n\nPrepare the game files for Xbox Series X|S?\n"
                           "This runs once (~10-20 seconds) and caches the linked boot image in LocalState.",
                       {"Prepare & Launch", "Cancel"}) != 0) {
            close_app("game preparation cancelled");
        }
        std::printf("%s Bloodborne boot image from %s...\n",
                    upgrading_cache ? "Upgrading cached" : "Preparing", dump.c_str());
        std::fflush(stdout);
        const auto t0 = std::chrono::steady_clock::now();
        BbPrepare::BootPrepared res;
        try {
            const char* rz_env = std::getenv("BB_RED_ZONE");
            const bool redzone =
                patch_state != PatchState::Skipped && !(rz_env && std::strcmp(rz_env, "0") == 0);
            res = BbPrepare::PrepareBoot(files, redzone_json, redzone);
        } catch (const std::exception& ex) {
            stop_with_message(std::string("Preparing Bloodborne failed:\n") + ex.what(), player);
            return false;
        }
        DeleteFileFromAppW(winrt::to_hstring(format_path).c_str());
        const BbPrepare::Bytes ver_bytes(res.app_ver.begin(), res.app_ver.end());
        if (!write_file_bytes(boot_path, res.boot_linked) ||
            !write_file_bytes(content_path, res.content) ||
            !write_file_bytes(eboot_elf_path, res.eboot_elf) ||
            !write_file_bytes(appver_path, ver_bytes) ||
            !write_file_bytes(format_path,
                              BbPrepare::Bytes(marker, marker + std::strlen(marker)))) {
            stop_with_message("Failed to save prepared Bloodborne files to LocalState\\Prepared.",
                              player);
            return false;
        }
        eboot_elf = std::move(res.eboot_elf);
        app_ver = res.app_ver;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        std::printf("Prepared %s (%s v%s) in %lld ms\n", res.title_id.c_str(), res.title.c_str(),
                    res.app_ver.c_str(), static_cast<long long>(ms));
    }

    // Release large raw dump buffers before compiling startup patches and launching the guest.
    files = {};
    set_env_var("BB_APP_VER", app_ver);

    const auto ini = parse_kv_file(ini_path);
    const std::string render_res_override = resolve_render_res_override(ini);
    std::string fps_mode = "60";
    if (const char* forced_fps = std::getenv("BB_FPS"); forced_fps && *forced_fps) {
        fps_mode = forced_fps;
    } else if (auto patch_it = ini.find("fps_patch");
               patch_it != ini.end() && !patch_it->second.empty()) {
        const std::string& p = patch_it->second;
        if (p == "30" || p == "60" || p == "90" || p == "uncap") {
            fps_mode = p;
        }
    } else if (auto it = ini.find("fps_cap"); it != ini.end()) {
        const int cap = std::atoi(it->second.c_str());
        if (cap > 0 && cap <= 30) {
            fps_mode = "30";
        }
    }
    const char* force_patches_env = std::getenv("BB_FORCE_PATCHES");
    const bool force_patches = force_patches_env && std::strcmp(force_patches_env, "1") == 0;

    try {
        auto patch_res = BbPrepare::BuildPatches(eboot_elf, app_ver, patch_xml, ini, fps_mode,
                                                 render_res_override, force_patches);
        if (!write_file_bytes(prepared + "\\patches.bin", patch_res.patches_bin)) {
            stop_with_message("Failed to write Prepared\\patches.bin.", player);
            return false;
        }
        if (patch_res.render_width != 1920 || patch_res.render_height != 1080) {
            set_env_var("BB_RENDER_RES", std::to_string(patch_res.render_width) + "x" +
                                             std::to_string(patch_res.render_height));
        } else {
            set_env_var("BB_RENDER_RES", "");
        }
        std::printf("Compiled startup patches: %s (%zu bytes)\n", patch_res.summary.c_str(),
                    patch_res.patches_bin.size());
    } catch (const std::exception& ex) {
        stop_with_message(std::string("Compiling Bloodborne patches failed:\n") + ex.what(),
                          player);
        return false;
    }

    return true;
}

static void configure_dlc_and_mods(const std::string& dump, const std::string& local_str,
                                   const std::string& user_str) {
    const std::string mods_local = local_str + "\\mods";
    CreateDirectoryFromAppW(winrt::to_hstring(mods_local).c_str(), nullptr);
    const std::string mods_dump = dump + "\\mods";
    set_env_var("BB_MODS_DIR", dir_exists(mods_dump) ? mods_dump : mods_local);

    const std::string dlc_local = local_str + "\\dlc";
    CreateDirectoryFromAppW(winrt::to_hstring(dlc_local).c_str(), nullptr);
    CreateDirectoryFromAppW(winrt::to_hstring(user_str + "\\addcont").c_str(), nullptr);

    std::string parent_dlc;
    if (const auto slash = dump.find_last_of("\\/"); slash != std::string::npos) {
        parent_dlc = dump.substr(0, slash) + "\\Bloodborne-DLC";
    }
    const std::string candidates[] = {
        dump + "\\dlc",
        dump + "\\addcont",
        parent_dlc,
        dlc_local,
    };
    for (const auto& c : candidates) {
        if (!c.empty() && dir_exists(c)) {
            set_env_var("BB_DLC_DIR", c);
            std::printf("DLC root configured: %s\n", c.c_str());
            break;
        }
    }
}

static std::wstring g_quarantine_path_w;

static LONG CALLBACK uwp_last_chance_logger(PEXCEPTION_POINTERS ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW &&
        code != 0xC0000409u) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    uint32_t diag_idx = 0, diag_total = 0, diag_stage = 0;
    uint64_t diag_hash = 0, diag_partner = 0;
    const bool in_precompile =
        bbgpu_uwp_active_precompile_diag(&diag_idx, &diag_total, &diag_stage, &diag_hash,
                                         &diag_partner) != 0;

    static std::atomic<uint32_t> s_logged{0};
    if (s_logged.fetch_add(1, std::memory_order_relaxed) < 8) {
        const uintptr_t rip = ep->ContextRecord->Rip;
        const uintptr_t rsp = ep->ContextRecord->Rsp;
        MEMORY_BASIC_INFORMATION mbi{};
        uintptr_t mod_base = 0;
        if (VirtualQuery(reinterpret_cast<LPCVOID>(rip), &mbi, sizeof(mbi))) {
            mod_base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
        }
        const char* mod_name = "unknown";
        if (mod_base && mod_base == reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))) {
            mod_name = "bloodborne_xbox.exe";
        } else if (mod_base &&
                   mod_base == reinterpret_cast<uintptr_t>(GetModuleHandleW(L"vulkan_dzn.dll"))) {
            mod_name = "vulkan_dzn.dll";
        } else if (mod_base &&
                   mod_base == reinterpret_cast<uintptr_t>(GetModuleHandleW(L"dxil.dll"))) {
            mod_name = "dxil.dll";
        }
        const uintptr_t rva = mod_base ? (rip - mod_base) : rip;
        const uintptr_t fault_addr =
            (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
                ? ep->ExceptionRecord->ExceptionInformation[1]
                : 0;
        if (in_precompile) {
            std::printf("UNHANDLED PRECOMPILE FAULT: code=0x%08lx module=%s+0x%llx rip=0x%llx "
                        "rsp=0x%llx addr=0x%llx item=%u/%u stage=%u hash=%016llx partner=%016llx\n",
                        static_cast<unsigned long>(code), mod_name,
                        static_cast<unsigned long long>(rva), static_cast<unsigned long long>(rip),
                        static_cast<unsigned long long>(rsp),
                        static_cast<unsigned long long>(fault_addr), diag_idx, diag_total,
                        diag_stage, static_cast<unsigned long long>(diag_hash),
                        static_cast<unsigned long long>(diag_partner));
        } else {
            std::printf("UNHANDLED NATIVE FAULT: code=0x%08lx module=%s+0x%llx rip=0x%llx "
                        "rsp=0x%llx addr=0x%llx\n",
                        static_cast<unsigned long>(code), mod_name,
                        static_cast<unsigned long long>(rva), static_cast<unsigned long long>(rip),
                        static_cast<unsigned long long>(rsp),
                        static_cast<unsigned long long>(fault_addr));
        }
        std::fflush(stdout);
    }

    if (in_precompile && diag_hash != 0 && !g_quarantine_path_w.empty()) {
        HANDLE qf = CreateFile2FromAppW(g_quarantine_path_w.c_str(), FILE_APPEND_DATA,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS, nullptr);
        if (qf != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER zero{};
            SetFilePointerEx(qf, zero, nullptr, FILE_END);
            char line[96]{};
            const int len = std::snprintf(line, sizeof(line), "%016llx\n",
                                          static_cast<unsigned long long>(diag_hash));
            DWORD written = 0;
            if (len > 0) {
                WriteFile(qf, line, static_cast<DWORD>(len), &written, nullptr);
            }
            if (diag_partner != 0) {
                const int len2 = std::snprintf(line, sizeof(line), "%016llx\n",
                                               static_cast<unsigned long long>(diag_partner));
                if (len2 > 0) {
                    WriteFile(qf, line, static_cast<DWORD>(len2), &written, nullptr);
                }
            }
            CloseHandle(qf);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// The guest memory contract self-test (xbox/platform/guest_memory_test.inc) runs through the
// loader's own entry point, so the fault handler the loader installs resolves the
// commit-on-touch pages the test walks, exactly as it does for the game.
static int run_memory_selftest() {
    char app_name[] = "bloodborne-uwp";
    char selftest_flag[] = "--memory-selftest";
    char* args[] = {app_name, selftest_flag};
    return bbport_uwp_run_probe(2, args);
}

static void run_loader() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    const auto local_w =
        std::wstring(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path());
    const std::string local_str = winrt::to_string(local_w);
    g_quarantine_path_w = local_w + L"\\precompile_quarantine.txt";

    {
        std::ifstream limit_file(local_w + L"\\memory_limit_mb.txt");
        uint64_t mb = 0;
        if (limit_file >> mb && mb >= 2048 && mb <= 65536) {
            g_memory_limit_override = mb << 20;
        }
    }

    {
        auto rotate_log = [&](const wchar_t* name, const wchar_t* prev_name) {
            std::error_code ec;
            const std::filesystem::path cur = std::filesystem::path{local_w} / name;
            const std::filesystem::path prev = std::filesystem::path{local_w} / prev_name;
            if (std::filesystem::exists(cur, ec) && std::filesystem::file_size(cur, ec) > 0 && !ec) {
                std::filesystem::remove(prev, ec);
                ec.clear();
                std::filesystem::rename(cur, prev, ec);
            }
        };
        rotate_log(L"loader.log", L"loader.prev.log");
        rotate_log(L"errors.log", L"errors.prev.log");
        rotate_log(L"dozen.log", L"dozen.prev.log");
    }

    _wfreopen((local_w + L"\\loader.log").c_str(), L"w", stdout);
    _wfreopen((local_w + L"\\errors.log").c_str(), L"w", stderr);
    HANDLE dozen_stream = CreateFile2FromAppW(
        (local_w + L"\\dozen.log").c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, CREATE_ALWAYS, nullptr);
    if (dozen_stream != INVALID_HANDLE_VALUE) {
        SetStdHandle(STD_ERROR_HANDLE, dozen_stream);
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    AddVectoredExceptionHandler(0, &uwp_last_chance_logger);

    std::printf("Bloodborne Xbox Series X|S UWP loader started\n");
    GAMING_DEVICE_MODEL_INFORMATION model{};
    GetGamingDeviceModelInformation(&model);
    std::printf("Device model: 0x%08x; app memory budget: %llu MiB; gamepads: %u\n", model.deviceId,
                static_cast<unsigned long long>(bb_uwp_app_memory_limit() >> 20),
                winrt::Windows::Gaming::Input::Gamepad::Gamepads().Size());

    // Verify dynamic code generation + System V AMD64 ABI invocation inside AppContainer.
    void* smoke_page = VirtualAllocFromApp(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old_prot = 0;
    if (!smoke_page) {
        std::printf("STOP: VirtualAllocFromApp smoke failed (%lu)\n", GetLastError());
        g_finished = true;
        return;
    }
    const unsigned char code_bytes[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};
    std::memcpy(smoke_page, code_bytes, sizeof(code_bytes));
    if (!VirtualProtectFromApp(smoke_page, 4096, PAGE_EXECUTE_READ, &old_prot)) {
        std::printf("STOP: VirtualProtectFromApp(PAGE_EXECUTE_READ) failed (%lu) — check codeGeneration capability\n",
                    GetLastError());
        VirtualFree(smoke_page, 0, MEM_RELEASE);
        g_finished = true;
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), smoke_page, 4096);
    using SysvSmokeFn = int(__attribute__((sysv_abi))*)();
    const int smoke_val = reinterpret_cast<SysvSmokeFn>(smoke_page)();
    VirtualFree(smoke_page, 0, MEM_RELEASE);
    std::printf("AppContainer codeGeneration + sysv_abi smoke: %s (%d)\n",
                smoke_val == 42 ? "PASS" : "FAIL", smoke_val);
    if (smoke_val != 42) {
        g_finished = true;
        return;
    }

    // Optional developer override file LocalState\run.txt (line 1: mode, line 2: dump path).
    std::ifstream run_cfg(local_w + L"\\run.txt");
    const bool player = !run_cfg.is_open();
    std::string mode = player ? "boot" : "";
    std::string dump;
    if (!player) {
        std::getline(run_cfg, mode);
        std::getline(run_cfg, dump);
        if (!mode.empty() && mode.back() == '\r') mode.pop_back();
        if (!dump.empty() && dump.back() == '\r') dump.pop_back();
    }

    // Optional LocalState\graphics-debug.txt env overrides.
    for (const auto& [k, v] : parse_kv_file(local_str + "\\graphics-debug.txt")) {
        if (k.rfind("BB_", 0) == 0 || k.rfind("DZN_", 0) == 0 || k.rfind("MESA_", 0) == 0) {
            set_env_var(k.c_str(), v);
        }
    }

    g_game_mode = (mode == "boot");
    set_env_var("BB_UWP_LAUNCHER", mode == "boot" ? "1" : "0");

    if (mode == "drives") {
        for (char c = 'A'; c <= 'Z'; ++c) {
            const std::string root = std::string(1, c) + ":\\";
            WIN32_FIND_DATAA entry{};
            HANDLE h = bb_uwp_find((root + "*").c_str(), &entry);
            if (h == INVALID_HANDLE_VALUE) continue;
            std::printf("Drive %s accessible\n", root.c_str());
            FindClose(h);
        }
        g_finished = true;
        return;
    }

    // Directories and variables the self-test and the game both need. They are set before the game
    // folder is chosen: the guest memory self-test runs on the first launch, before the folder
    // picker opens (that dialog waits for the player).
    const std::string user_str = local_str + "\\user";
    const std::string prepared_str = local_str + "\\Prepared";
    const std::string temp_str = local_str + "\\temp";
    const std::string ini_path = local_str + "\\bbport.ini";

    CreateDirectoryFromAppW(winrt::to_hstring(user_str).c_str(), nullptr);
    CreateDirectoryFromAppW(winrt::to_hstring(prepared_str).c_str(), nullptr);
    CreateDirectoryFromAppW(winrt::to_hstring(temp_str).c_str(), nullptr);

    set_env_var("BB_USER_DIR", user_str);
    set_env_var("BB_GPU_USER_DIR", user_str);
    set_env_var("TEMP", temp_str);
    set_env_var("TMP", temp_str);
    set_env_var("BB_UWP_SWAP_FILE", local_str + "\\bloodborne-swap.bin");

    // Guest memory contract self-test (cosmo/gow3_xbox). It runs by itself on the first launch
    // after install (while LocalState\memory-selftest.txt does not exist), and again whenever
    // LocalState\graphics-debug.txt sets BB_MEMORY_SELFTEST=1 or LocalState\run.txt starts with
    // "memory". Nothing has to be created on the console to get the result: it goes to
    // loader.log.txt and to memory-selftest.txt. The marker is written before the test runs, so a
    // test that hangs is not repeated on every start.
    {
        const std::filesystem::path marker_path =
            std::filesystem::path{local_w} / L"memory-selftest.txt";
        std::error_code marker_ec;
        const bool ran_before = std::filesystem::exists(marker_path, marker_ec) && !marker_ec;
        const char* forced = std::getenv("BB_MEMORY_SELFTEST");
        const bool requested = mode == "memory" || (forced && *forced == '1');
        if (requested || !ran_before) {
            std::printf("UWP memory: guest memory contract self-test starting (%s)\n",
                        requested ? "requested" : "first launch");
            std::fflush(stdout);
            const auto write_marker = [&marker_path](const std::string& text) {
                std::ofstream marker{marker_path, std::ios::trunc};
                marker << text;
            };
            write_marker("started\n");
            const int result = run_memory_selftest();
            write_marker("result=" + std::to_string(result) + "\n");
            std::printf("UWP memory: contract self-test %s (result %d)\n",
                        result == 0 ? "PASSED" : "FAILED", result);
            std::fflush(stdout);
            if (result != 0) {
                // A failed check leaves guest mappings and swap state half-restored: the game is
                // not started on this launch, so the failure stays readable instead of turning
                // into a crash inside the loader. The marker is already written, so the next
                // launch goes straight to the game.
                std::printf("UWP memory: close the app and launch it again to start the game "
                            "without the test (it runs once; set BB_MEMORY_SELFTEST=1 in "
                            "graphics-debug.txt to repeat it)\n");
                std::fflush(stdout);
                g_finished = true;
                return;
            }
        }
        if (mode == "memory") {
            g_finished = true;
            return;
        }
    }

    if (player) {
        const char* force_choose = std::getenv("BB_CHOOSE_DUMP");
        const bool choose = force_choose && *force_choose == '1';
        if (!choose) dump = chosen_dump();
        if (!choose && dump.empty()) {
            auto usb = usb_dumps();
            if (!usb.empty()) dump = usb.front();
        }
        if (!choose && dump.empty()) {
            auto internal = internal_dumps();
            if (!internal.empty()) dump = internal.front();
        }
        if (!choose && dump.empty()) {
            auto local_d = localstate_dumps();
            if (!local_d.empty()) dump = local_d.front();
        }
        if (dump.empty()) {
            dump = choose_dump_loop(
                choose ? "Choose your decrypted PS4 Bloodborne dump folder."
                       : "No Bloodborne game dump found yet.\n\n"
                         "Copy your decrypted PS4 Bloodborne v1.09 dump (the folder containing "
                         "eboot.bin, sce_module, sce_sys, and dvdroot_ps4) to:\n"
                         "  - USB drive: E:\\dumps\\Bloodborne (or E:\\Bloodborne)\n"
                         "  - Console Dev Mode storage: D:\\DevelopmentFiles\\Bloodborne\n\n"
                         "Or choose the folder now:");
        }
        if (dump.empty()) {
            close_app("no game folder selected");
        }
        std::printf("Picked Bloodborne folder: %s (%s)\n", dump.c_str(),
                    describe_place(dump).c_str());
    } else if (dump.empty() && mode != "memory") {
        std::puts("STOP: dump path missing on line 2 of LocalState\\run.txt");
        g_finished = true;
        return;
    }

    ensure_default_ini(ini_path);

    // Read the dump's metadata, pair a base game with a separately dumped update folder and let the
    // player confirm or change that choice before anything is prepared.
    DumpSelection selection;
    if (!launcher_select_dump_layout(dump, ini_path, player, selection)) {
        g_finished = true;
        return;
    }
    dump = selection.BasePath();
    std::printf("%s\n", BbPanel::LaunchTargetLine(PanelOf(selection),
                                                  selection.CanRunPatched() ? "required"
                                                                            : "skipped")
                            .c_str());
    std::printf("Active Bloodborne dump: %s (%s)\n", dump.c_str(), describe_place(dump).c_str());
    std::fflush(stdout);

    configure_dlc_and_mods(dump, local_str, user_str);

    set_env_var("BB_UWP_DUMP", dump);
    set_env_var("BB_GAME_DIR", dump);
    set_env_var("BB_CONFIG", ini_path);
    set_env_var("BB_UWP_PREPARED", prepared_str);
    set_default_env_var("BB_UPSCALER", "off");
    set_default_env_var("BB_FPS_LOG", "1");
    set_default_env_var("BB_PRECOMPILE", "0");
    set_default_env_var("BB_PREP_WORKERS", "0");

    if (!prepare_game_and_patches(dump, prepared_str, ini_path, player, mode == "prepare",
                                  selection) ||
        mode == "prepare") {
        g_finished = true;
        return;
    }

    if (mode == "boot") {
        runtime_memory_start_paging();
    }

    std::vector<std::string> arg_strings = {
        "bloodborne",
        prepared_str + "\\boot-linked.bin",
    };
    if (mode != "boot") {
        arg_strings.push_back("--cpu-only");
    }
    arg_strings.insert(arg_strings.end(), {
        "--app0", dump,
        "--user", user_str,
        "--content-profile", prepared_str + "\\content.bin",
        "--patches", prepared_str + "\\patches.bin",
    });
    if (mode == "imports") {
        arg_strings.push_back("--check-imports");
    }

    std::vector<char*> argv;
    argv.reserve(arg_strings.size());
    for (auto& s : arg_strings) {
        argv.push_back(s.data());
    }

    std::puts("Handing off to Bloodborne sysv_abi loader + shadPS4/Dozen GPU runtime...");
    std::fflush(stdout);
    const int rc = bbport_uwp_run_probe(static_cast<int>(argv.size()), argv.data());
    std::printf("Bloodborne loader returned %d\n", rc);
    g_finished = true;
}

static unsigned __stdcall loader_thread_entry(void*) {
    try {
        run_loader();
    } catch (const winrt::hresult_error& err) {
        std::printf("STOP: WinRT error 0x%08x (%s)\n", static_cast<unsigned>(err.code()),
                    winrt::to_string(err.message()).c_str());
        g_finished = true;
    } catch (const std::exception& err) {
        std::printf("STOP: unhandled exception: %s\n", err.what());
        g_finished = true;
    }
    std::fflush(nullptr);
    return 0;
}

struct View : winrt::implements<View, winrt::Windows::ApplicationModel::Core::IFrameworkView> {
    winrt::Windows::UI::Core::CoreWindow window{nullptr};

    void Initialize(const winrt::Windows::ApplicationModel::Core::CoreApplicationView& app) {
        app.Activated([this](const auto&, const auto&) { window.Activate(); });
        winrt::Windows::ApplicationModel::Core::CoreApplication::Suspending(
            [](const auto&, const winrt::Windows::ApplicationModel::SuspendingEventArgs& args) {
                auto deferral = args.SuspendingOperation().GetDeferral();
                std::printf("UWP lifecycle: suspending; memory=%llu/%llu MiB\n",
                            static_cast<unsigned long long>(bb_uwp_app_memory_usage() >> 20),
                            static_cast<unsigned long long>(bb_uwp_app_memory_limit() >> 20));
                bbgpu_flush_caches();
                std::fflush(nullptr);
                deferral.Complete();
            });
        winrt::Windows::ApplicationModel::Core::CoreApplication::Resuming(
            [](const auto&, const auto&) {
                std::puts("UWP lifecycle: resuming");
                std::fflush(stdout);
            });
    }

    void SetWindow(const winrt::Windows::UI::Core::CoreWindow& value) {
        window = value;
        g_core_window = winrt::agile_ref(value);

        window.Closed([](const auto&, const auto&) {
            if (!g_finished.load()) {
                std::puts("UWP lifecycle: CoreWindow closed");
                std::fflush(stdout);
            }
            g_window_closed = true;
        });

        winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView().BackRequested(
            [](const auto&, const winrt::Windows::UI::Core::BackRequestedEventArgs& ev) {
                ev.Handled(true);
            });

        window.VisibilityChanged([](const auto&, const auto& ev) {
            const bool vis = ev.Visible();
            g_app_visible.store(vis, std::memory_order_relaxed);
            std::printf("UWP lifecycle: visible=%u\n", unsigned(vis));
            std::fflush(stdout);
        });

        window.KeyDown([](const auto&, const auto& ev) {
            if (bb_uwp_dialog_key(uint32_t(ev.VirtualKey()), 1, ev.KeyStatus().WasKeyDown)) {
                ev.Handled(true);
            }
        });
        window.KeyUp([](const auto&, const auto& ev) {
            if (bb_uwp_dialog_key(uint32_t(ev.VirtualKey()), 0, 0)) {
                ev.Handled(true);
            }
        });
    }

    void Load(const winrt::hstring&) {}

    void Run() {
        // Spawn the Bloodborne guest main thread with a dedicated 16 MiB stack.
        uintptr_t th = _beginthreadex(nullptr, 16 * 1024 * 1024, &loader_thread_entry, nullptr, 0,
                                      nullptr);
        if (th) {
            CloseHandle(reinterpret_cast<HANDLE>(th));
        }

        // Log memory usage periodically and dump wait/thread stacks if frame presentation
        // ever stalls for >= 10 seconds while visible.
        std::thread([] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            unsigned entered_ticks = 0;
            bool auto_sampled = false;
            uint64_t last_presents = 0;
            unsigned stalled_ticks = 0;
            bool stall_dumped = false;
            for (unsigned sample = 0; !g_finished.load(); ++sample) {
                Sleep(5000);
                const uint64_t presents = bbgpu_uwp_present_count();
                if (sample < 24 || stalled_ticks > 0) {
                    // `guest pinned`: the guest pages the pager may not hand back. It grows with
                    // what the game touches and no eviction returns it, so it is the number that
                    // explains a usage line sitting at the limit while the texture cache is
                    // evicting.
                    std::printf(
                        "UWP memory telemetry: %llu/%llu MiB (level=%u, presents=%llu, "
                        "guest pinned=%llu MiB)\n",
                        static_cast<unsigned long long>(bb_uwp_app_memory_usage() >> 20),
                        static_cast<unsigned long long>(bb_uwp_app_memory_limit() >> 20),
                        unsigned(winrt::Windows::System::MemoryManager::AppMemoryUsageLevel()),
                        static_cast<unsigned long long>(presents),
                        static_cast<unsigned long long>(runtime_memory_pinned_bytes() >> 20));
                }
                if (runtime_entered_game()) {
                    if (!auto_sampled && ++entered_ticks >= 6) {
                        auto_sampled = true;
                        std::puts("UWP +30s boot health snapshot:");
                        runtime_memory_report();
                        runtime_file_report();
                        runtime_wait_report();
                        runtime_sample_threads();
                    }
                    if (g_app_visible.load(std::memory_order_relaxed) && presents > 0) {
                        if (presents == last_presents) {
                            if (++stalled_ticks >= 2 && !stall_dumped) {
                                stall_dumped = true;
                                std::printf(
                                    "UWP STALL DETECTED (presents stuck at %llu for %u s):\n",
                                    static_cast<unsigned long long>(presents), stalled_ticks * 5);
                                runtime_memory_report();
                                runtime_file_report();
                                runtime_audio_report();
                                runtime_pad_report();
                                runtime_wait_report();
                                runtime_sample_threads();
                            }
                        } else {
                            stalled_ticks = 0;
                            stall_dumped = false;
                        }
                    }
                }
                last_presents = presents;
                std::fflush(stdout);
            }
        }).detach();

        std::thread([diag = std::wstring(winrt::Windows::Storage::ApplicationData::Current()
                                             .LocalFolder()
                                             .Path()) +
                            L"\\diagnostics.txt",
                     threads = std::wstring(winrt::Windows::Storage::ApplicationData::Current()
                                                .LocalFolder()
                                                .Path()) +
                               L"\\threads.txt"] {
            WIN32_FILE_ATTRIBUTE_DATA attr{};
            DeleteFileFromAppW(diag.c_str());
            DeleteFileFromAppW(threads.c_str());
            for (;;) {
                if (GetFileAttributesExFromAppW(diag.c_str(), GetFileExInfoStandard, &attr)) {
                    DeleteFileFromAppW(diag.c_str());
                    runtime_memory_report();
                    runtime_file_report();
                    runtime_audio_report();
                    runtime_pad_report();
                    runtime_savedata_report();
                    runtime_wait_report();
                    runtime_sample_threads();
                    std::fflush(stdout);
                }
                if (GetFileAttributesExFromAppW(threads.c_str(), GetFileExInfoStandard, &attr)) {
                    DeleteFileFromAppW(threads.c_str());
                    runtime_wait_report();
                    runtime_sample_threads();
                    std::fflush(stdout);
                }
                Sleep(500);
            }
        }).detach();

        std::thread([quit = std::wstring(winrt::Windows::Storage::ApplicationData::Current()
                                             .LocalFolder()
                                             .Path()) +
                            L"\\quit.txt"] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            WIN32_FILE_ATTRIBUTE_DATA attr{};
            DeleteFileFromAppW(quit.c_str());
            while (!GetFileAttributesExFromAppW(quit.c_str(), GetFileExInfoStandard, &attr)) {
                Sleep(250);
            }
            DeleteFileFromAppW(quit.c_str());
            if (!g_finished.load()) {
                std::puts("UWP lifecycle: quit.txt trigger received");
                std::fflush(stdout);
            }
            close_app("quit.txt requested");
        }).detach();

        while (!g_window_closed.load()) {
            window.Dispatcher().ProcessEvents(
                winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessOneAndAllPending);
        }
        exit_now("window closed");
    }

    void Uninitialize() {}
};

struct ViewSource
    : winrt::implements<ViewSource, winrt::Windows::ApplicationModel::Core::IFrameworkViewSource> {
    winrt::Windows::ApplicationModel::Core::IFrameworkView CreateView() {
        return winrt::make<View>();
    }
};

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    winrt::init_apartment();
    winrt::Windows::ApplicationModel::Core::CoreApplication::Run(winrt::make<ViewSource>());
    return 0;
}
