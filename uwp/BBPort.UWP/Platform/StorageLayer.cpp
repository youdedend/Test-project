// SPDX-License-Identifier: GPL-2.0-or-later
#include "StorageLayer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#endif

namespace bbport::uwp {

namespace {

bool CopyStringSafe(const std::string& src, char* dst, size_t cap) {
    if (!dst || cap == 0) return false;
    if (src.size() + 1 > cap) {
        dst[0] = '\0';
        return false;
    }
    std::memcpy(dst, src.c_str(), src.size() + 1);
    return true;
}

std::string SanitizeCacheKey(const std::string& key) {
    std::string out;
    out.reserve(key.size());
    for (char c : key) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '_' || c == '-';
        out.push_back(safe ? c : '_');
    }
    return out.empty() ? "unnamed" : out;
}

} // namespace

StorageLayer& StorageLayer::Instance() {
    static StorageLayer inst;
    return inst;
}

bool StorageLayer::Initialize(const std::filesystem::path& override_local,
                              const std::filesystem::path& override_temp) {
    if (initialized_ && override_local.empty() && override_temp.empty()) {
        return true;
    }
    std::filesystem::path local_root = override_local;
    std::filesystem::path temp_root = override_temp;

#if defined(_WIN32)
    if (local_root.empty() || temp_root.empty()) {
        try {
            const auto app_data = winrt::Windows::Storage::ApplicationData::Current();
            if (local_root.empty()) {
                local_root = std::wstring_view(app_data.LocalFolder().Path());
            }
            if (temp_root.empty()) {
                temp_root = std::wstring_view(app_data.TemporaryFolder().Path());
            }
        } catch (...) {
            return false;
        }
    }
#endif

    if (local_root.empty() || temp_root.empty()) {
        return false;
    }

    roots_.local_folder     = local_root;
    roots_.temporary_folder = temp_root;
    roots_.game_app0_dir    = local_root / "game" / "CUSA03173";
    roots_.user_data_dir    = local_root / "user";
    roots_.save_data_dir    = roots_.user_data_dir / "savedata" / "1" / "CUSA03173" / "SPRJ0005";
    roots_.shader_cache_dir = temp_root / "shader_cache" / "d3d12";
    roots_.temp_guest_dir   = temp_root / "temp0";

    std::error_code ec;
    std::filesystem::create_directories(roots_.game_app0_dir, ec);
    std::filesystem::create_directories(roots_.user_data_dir / "data", ec);
    std::filesystem::create_directories(roots_.user_data_dir / "download0", ec);
    std::filesystem::create_directories(roots_.save_data_dir, ec);
    std::filesystem::create_directories(roots_.shader_cache_dir, ec);
    std::filesystem::create_directories(roots_.temp_guest_dir, ec);

    initialized_ = !ec;
    return initialized_;
}

const StorageRoots& StorageLayer::Roots() const {
    return roots_;
}

bool StorageLayer::ReadShaderCacheBlob(const std::string& cache_key, std::string* out_bytes) const {
    if (!initialized_ || !out_bytes) return false;
    const auto file_path = roots_.shader_cache_dir / (SanitizeCacheKey(cache_key) + ".dxil");
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs) return false;
    ifs.seekg(0, std::ios::end);
    const auto len = ifs.tellg();
    if (len <= 0) return false;
    out_bytes->resize(static_cast<size_t>(len));
    ifs.seekg(0, std::ios::beg);
    ifs.read(out_bytes->data(), len);
    return ifs.good() || ifs.eof();
}

bool StorageLayer::WriteShaderCacheBlob(const std::string& cache_key,
                                        const void* data,
                                        size_t size) const {
    if (!initialized_ || (!data && size > 0)) return false;
    const auto final_path = roots_.shader_cache_dir / (SanitizeCacheKey(cache_key) + ".dxil");
    const auto tmp_path   = roots_.shader_cache_dir / (SanitizeCacheKey(cache_key) + ".dxil.tmp");
    {
        std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        if (size > 0) {
            ofs.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        }
        if (!ofs.good()) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp_path, final_path, ec);
    if (ec) {
        std::filesystem::remove(final_path, ec);
        ec.clear();
        std::filesystem::rename(tmp_path, final_path, ec);
    }
    return !ec;
}

uint64_t StorageLayer::PruneShaderCache(uint64_t max_bytes) const {
    if (!initialized_) return 0;
    struct EntryInfo {
        std::filesystem::path path;
        uint64_t size = 0;
        std::filesystem::file_time_type mtime{};
    };
    std::vector<EntryInfo> entries;
    uint64_t total_bytes = 0;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(roots_.shader_cache_dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const uint64_t sz = entry.file_size(ec);
        const auto mt = entry.last_write_time(ec);
        if (!ec) {
            entries.push_back({entry.path(), sz, mt});
            total_bytes += sz;
        }
    }
    if (total_bytes <= max_bytes) return 0;

    std::sort(entries.begin(), entries.end(), [](const EntryInfo& a, const EntryInfo& b) {
        return a.mtime < b.mtime;
    });

    uint64_t freed = 0;
    for (const auto& item : entries) {
        if (total_bytes - freed <= max_bytes) break;
        if (std::filesystem::remove(item.path, ec) && !ec) {
            freed += item.size;
        }
    }
    return freed;
}

bool StorageLayer::PrepareSaveDirectoryAndSoundFlag() const {
    if (!initialized_) return false;
    std::error_code ec;
    std::filesystem::create_directories(roots_.save_data_dir, ec);
    if (ec) return false;

    // Remove interrupted *.bbtmp atomic-commit files (matching src/runtime_file.c).
    for (const auto& entry : std::filesystem::directory_iterator(roots_.save_data_dir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".bbtmp") {
            std::filesystem::remove(entry.path(), ec);
        }
    }

    // Apply Bloodborne userdata0010 offset 0x204E sound-flag fix if a save already exists.
    const auto userdata10 = roots_.save_data_dir / "userdata0010";
    if (std::filesystem::exists(userdata10, ec) &&
        std::filesystem::file_size(userdata10, ec) > 0x204Eu) {
        std::fstream fs(userdata10, std::ios::in | std::ios::out | std::ios::binary);
        if (fs) {
            fs.seekg(0x204E, std::ios::beg);
            char flag = 0;
            if (fs.get(flag) && flag != 1) {
                fs.seekp(0x204E, std::ios::beg);
                const char enabled = 1;
                fs.write(&enabled, 1);
            }
        }
    }
    return true;
}

void StorageLayer::SaveSettings(const LauncherPersistentSettings& s) {
#if defined(_WIN32)
    try {
        auto values = winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        values.Insert(L"target_fps", winrt::box_value(s.target_fps));
        values.Insert(L"render_preset", winrt::box_value(s.render_preset));
        values.Insert(L"output_height", winrt::box_value(s.output_height));
        values.Insert(L"swap_confirm_circle", winrt::box_value(s.swap_confirm_circle));
        values.Insert(L"enable_sound_hack", winrt::box_value(s.enable_sound_hack));
        values.Insert(L"enable_online_shadnet", winrt::box_value(s.enable_online_shadnet));
        values.Insert(L"shadnet_server", winrt::box_value(winrt::to_hstring(s.shadnet_server)));
        values.Insert(L"shadnet_user", winrt::box_value(winrt::to_hstring(s.shadnet_user)));
        values.Insert(L"shader_compile_threads", winrt::box_value(s.shader_compile_threads));
        values.Insert(L"dmem_budget_mb", winrt::box_value(s.dmem_budget_mb));
    } catch (...) {
    }
#endif
    if (!initialized_) return;
    const auto ini_path = roots_.local_folder / "bbport_uwp.ini";
    std::ofstream ofs(ini_path, std::ios::trunc);
    if (!ofs) return;
    ofs << "target_fps=" << s.target_fps << "\n"
        << "render_preset=" << s.render_preset << "\n"
        << "output_height=" << s.output_height << "\n"
        << "swap_confirm_circle=" << (s.swap_confirm_circle ? 1 : 0) << "\n"
        << "enable_sound_hack=" << (s.enable_sound_hack ? 1 : 0) << "\n"
        << "enable_online_shadnet=" << (s.enable_online_shadnet ? 1 : 0) << "\n"
        << "shadnet_server=" << s.shadnet_server << "\n"
        << "shadnet_user=" << s.shadnet_user << "\n"
        << "shader_compile_threads=" << s.shader_compile_threads << "\n"
        << "dmem_budget_mb=" << s.dmem_budget_mb << "\n";
}

LauncherPersistentSettings StorageLayer::LoadSettings() const {
    LauncherPersistentSettings s{};
#if defined(_WIN32)
    try {
        auto values = winrt::Windows::Storage::ApplicationData::Current().LocalSettings().Values();
        if (values.HasKey(L"target_fps")) s.target_fps = winrt::unbox_value<int>(values.Lookup(L"target_fps"));
        if (values.HasKey(L"render_preset")) s.render_preset = winrt::unbox_value<int>(values.Lookup(L"render_preset"));
        if (values.HasKey(L"output_height")) s.output_height = winrt::unbox_value<int>(values.Lookup(L"output_height"));
        if (values.HasKey(L"swap_confirm_circle")) s.swap_confirm_circle = winrt::unbox_value<bool>(values.Lookup(L"swap_confirm_circle"));
        if (values.HasKey(L"enable_sound_hack")) s.enable_sound_hack = winrt::unbox_value<bool>(values.Lookup(L"enable_sound_hack"));
        if (values.HasKey(L"enable_online_shadnet")) s.enable_online_shadnet = winrt::unbox_value<bool>(values.Lookup(L"enable_online_shadnet"));
        if (values.HasKey(L"shadnet_server")) s.shadnet_server = winrt::to_string(winrt::unbox_value<winrt::hstring>(values.Lookup(L"shadnet_server")));
        if (values.HasKey(L"shadnet_user")) s.shadnet_user = winrt::to_string(winrt::unbox_value<winrt::hstring>(values.Lookup(L"shadnet_user")));
        if (values.HasKey(L"shader_compile_threads")) s.shader_compile_threads = winrt::unbox_value<uint32_t>(values.Lookup(L"shader_compile_threads"));
        if (values.HasKey(L"dmem_budget_mb")) s.dmem_budget_mb = winrt::unbox_value<uint32_t>(values.Lookup(L"dmem_budget_mb"));
        return s;
    } catch (...) {
    }
#endif
    if (!initialized_) return s;
    std::ifstream ifs(roots_.local_folder / "bbport_uwp.ini");
    std::string line;
    while (std::getline(ifs, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const std::string v = line.substr(eq + 1);
        if (k == "target_fps") s.target_fps = std::atoi(v.c_str());
        else if (k == "render_preset") s.render_preset = std::atoi(v.c_str());
        else if (k == "output_height") s.output_height = std::atoi(v.c_str());
        else if (k == "swap_confirm_circle") s.swap_confirm_circle = (std::atoi(v.c_str()) != 0);
        else if (k == "enable_sound_hack") s.enable_sound_hack = (std::atoi(v.c_str()) != 0);
        else if (k == "enable_online_shadnet") s.enable_online_shadnet = (std::atoi(v.c_str()) != 0);
        else if (k == "shadnet_server") s.shadnet_server = v;
        else if (k == "shadnet_user") s.shadnet_user = v;
        else if (k == "shader_compile_threads") s.shader_compile_threads = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        else if (k == "dmem_budget_mb") s.dmem_budget_mb = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
    }
    return s;
}

} // namespace bbport::uwp

extern "C" int bbport_uwp_storage_get_roots(char* app0_out, size_t app0_cap,
                                            char* user_out, size_t user_cap,
                                            char* temp_out, size_t temp_cap) {
    auto& storage = bbport::uwp::StorageLayer::Instance();
    if (!storage.Initialize()) {
        return -1;
    }
    const auto& r = storage.Roots();
    const std::string app0 = r.game_app0_dir.generic_string();
    const std::string user = r.user_data_dir.generic_string();
    const std::string temp = r.temp_guest_dir.generic_string();
    if (app0_out && app0_cap > 0 && !bbport::uwp::CopyStringSafe(app0, app0_out, app0_cap)) {
        return -1;
    }
    if (user_out && user_cap > 0 && !bbport::uwp::CopyStringSafe(user, user_out, user_cap)) {
        return -1;
    }
    if (temp_out && temp_cap > 0 && !bbport::uwp::CopyStringSafe(temp, temp_out, temp_cap)) {
        return -1;
    }
    return 0;
}
