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

#include "prepare_game.h"

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
void runtime_file_report(void);
void runtime_audio_report(void);
void runtime_pad_report(void);
void runtime_savedata_report(void);
void runtime_wait_report(void);
void runtime_sample_threads(void);
int runtime_entered_game(void);
int bb_uwp_dialog_key(uint32_t key, int down, int repeat);
uint64_t bbgpu_uwp_present_count(void);
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

static void set_env_var(const char* name, const std::string& value) {
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableW(winrt::to_hstring(name).c_str(), winrt::to_hstring(value).c_str());
}

static int ask_player(const std::string& text, const std::vector<std::string>& buttons) {
    std::printf("Player dialog: %s\n", text.c_str());
    auto choice = std::make_shared<std::promise<int>>();
    auto answer = choice->get_future();
    g_core_window.get().Dispatcher().RunAsync(
        winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [text, buttons, choice] {
            try {
                winrt::Windows::UI::Popups::MessageDialog dialog(winrt::to_hstring(text),
                                                                 L"Bloodborne");
                for (size_t i = 0; i < buttons.size(); ++i) {
                    dialog.Commands().Append(winrt::Windows::UI::Popups::UICommand(
                        winrt::to_hstring(buttons[i]), nullptr, winrt::box_value(int(i))));
                }
                dialog.DefaultCommandIndex(0);
                dialog.CancelCommandIndex(uint32_t(buttons.size() - 1));
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
        "upscaler=off\n"
        "preset=1\n"
        "show_fps=1\n"
        "button_prompts=xbox\n"
        "quit_to_desktop=0\n"
        "rebirth=1\n"
        "effect_chromatic_aberration=0\n"
        "effect_dof=1\n"
        "effect_motion_blur=0\n"
        "effect_ssao=1\n"
        "effect_game_aa=1\n"
        "effect_dynamic_shadows=1\n"
        "effect_ssr=0\n"
        "model_lod=0\n"
        "output_res=1920x1080\n"
        "fov=43\n"
        "fps_cap=60\n";
    BbPrepare::Bytes bytes(defaults, defaults + std::strlen(defaults));
    write_file_bytes(ini_path, bytes);
}

static std::map<std::string, std::string> parse_kv_file(const std::string& path) {
    std::map<std::string, std::string> kv;
    BbPrepare::Bytes raw;
    if (!read_file_bytes(path, raw)) {
        return kv;
    }
    std::string content(raw.begin(), raw.end());
    size_t pos = 0;
    while (pos < content.size()) {
        size_t end = content.find('\n', pos);
        if (end == std::string::npos) end = content.size();
        std::string line = content.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
        kv[k] = v;
    }
    return kv;
}

static std::string resolve_render_res_override(const std::map<std::string, std::string>& ini) {
    if (const char* forced = std::getenv("BB_UWP_RENDER_RES"); forced && *forced) {
        return forced;
    }
    auto w_it = ini.find("render_width");
    auto h_it = ini.find("render_height");
    if (w_it != ini.end() && h_it != ini.end()) {
        const int w = std::atoi(w_it->second.c_str());
        const int h = std::atoi(h_it->second.c_str());
        if (w >= 480 && h >= 270) {
            return std::to_string(w) + "x" + std::to_string(h);
        }
    }
    return {};
}

static bool prepare_game_and_patches(const std::string& dump, const std::string& prepared,
                                     const std::string& ini_path, bool player, bool force_rebuild) {
    const std::string pkg_path = winrt::to_string(
        winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path());

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
        if (!read_file_bytes(dump + "\\" + rel, *dest) || dest->empty()) {
            stop_with_message("The Bloodborne dump in " + dump + " is incomplete:\n" + rel +
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
            const bool redzone = !(rz_env && std::strcmp(rz_env, "0") == 0);
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

    const auto ini = parse_kv_file(ini_path);
    const std::string render_res_override = resolve_render_res_override(ini);
    std::string fps_mode = "60";
    if (const char* forced_fps = std::getenv("BB_FPS"); forced_fps && *forced_fps) {
        fps_mode = forced_fps;
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
        std::printf("Active Bloodborne dump: %s (%s)\n", dump.c_str(),
                    describe_place(dump).c_str());
    } else if (dump.empty()) {
        std::puts("STOP: dump path missing on line 2 of LocalState\\run.txt");
        g_finished = true;
        return;
    }

    const std::string user_str = local_str + "\\user";
    const std::string prepared_str = local_str + "\\Prepared";
    const std::string temp_str = local_str + "\\temp";
    const std::string ini_path = local_str + "\\bbport.ini";

    CreateDirectoryFromAppW(winrt::to_hstring(user_str).c_str(), nullptr);
    CreateDirectoryFromAppW(winrt::to_hstring(prepared_str).c_str(), nullptr);
    CreateDirectoryFromAppW(winrt::to_hstring(temp_str).c_str(), nullptr);

    ensure_default_ini(ini_path);
    configure_dlc_and_mods(dump, local_str, user_str);

    set_env_var("BB_UWP_DUMP", dump);
    set_env_var("BB_UWP_PREPARED", prepared_str);
    set_env_var("BB_GAME_DIR", dump);
    set_env_var("BB_USER_DIR", user_str);
    set_env_var("BB_GPU_USER_DIR", user_str);
    set_env_var("BB_CONFIG", ini_path);
    set_env_var("TEMP", temp_str);
    set_env_var("TMP", temp_str);
    set_env_var("BB_UWP_SWAP_FILE", local_str + "\\bloodborne-swap.bin");
    set_env_var("BB_UPSCALER", "off");
    set_env_var("BB_FPS_LOG", "1");

    if (!prepare_game_and_patches(dump, prepared_str, ini_path, player, mode == "prepare") ||
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
                    std::printf(
                        "UWP memory telemetry: %llu/%llu MiB (level=%u, presents=%llu)\n",
                        static_cast<unsigned long long>(bb_uwp_app_memory_usage() >> 20),
                        static_cast<unsigned long long>(bb_uwp_app_memory_limit() >> 20),
                        unsigned(winrt::Windows::System::MemoryManager::AppMemoryUsageLevel()),
                        static_cast<unsigned long long>(presents));
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
