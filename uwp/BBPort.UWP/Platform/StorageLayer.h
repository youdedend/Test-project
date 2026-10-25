// SPDX-License-Identifier: GPL-2.0-or-later
// UWP Sandbox Storage Layer: routes game assets & saves to ApplicationData.Current.LocalFolder,
// DXIL/PSO shader caches to ApplicationData.Current.TemporaryFolder, and settings to LocalSettings.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace bbport::uwp {

struct StorageRoots {
    std::filesystem::path local_folder;      // ApplicationData::Current().LocalFolder().Path()
    std::filesystem::path temporary_folder;  // ApplicationData::Current().TemporaryFolder().Path()
    std::filesystem::path game_app0_dir;     // <LocalFolder>/game/CUSA03173
    std::filesystem::path user_data_dir;     // <LocalFolder>/user
    std::filesystem::path save_data_dir;     // <LocalFolder>/user/savedata/1/CUSA03173/SPRJ0005
    std::filesystem::path shader_cache_dir;  // <TemporaryFolder>/shader_cache/d3d12
    std::filesystem::path temp_guest_dir;    // <TemporaryFolder>/temp0
};

struct LauncherPersistentSettings {
    int target_fps = 60;                 // 30, 60, 90, 120
    int render_preset = 1;               // 0=NativeAA, 1=Quality, 2=Balanced, 3=Performance
    int output_height = 1080;            // 720, 1080, 1440, 2160
    bool swap_confirm_circle = false;    // Asian Circle=confirm
    bool enable_sound_hack = true;       // userdata0010 offset 0x204E audio fix
    bool enable_online_shadnet = false;  // shadNet / The Hunter's Dream beta
    std::string shadnet_server = "shadnet.shadps4.net";
    std::string shadnet_user;
    uint32_t shader_compile_threads = 2; // capped at 2 on Xbox Zen 2 to prevent frame hitches
    uint32_t dmem_budget_mb = 3328;      // fits inside Xbox Dev Mode 5,120 MiB Game Mode cap
};

class StorageLayer {
public:
    static StorageLayer& Instance();

    // Initializes sandbox directories; optional override_local / override_temp supports host unit tests.
    bool Initialize(const std::filesystem::path& override_local = {},
                    const std::filesystem::path& override_temp = {});

    const StorageRoots& Roots() const;

    // Writes/reads shader cache blobs in ApplicationData.Current.TemporaryFolder.
    bool ReadShaderCacheBlob(const std::string& cache_key, std::string* out_bytes) const;
    bool WriteShaderCacheBlob(const std::string& cache_key, const void* data, size_t size) const;
    uint64_t PruneShaderCache(uint64_t max_bytes) const;

    // Ensures Bloodborne's userdata0010 byte 0x204E is 1 (weapon SFX fix from src/runtime_savedata.c)
    // and cleans up any interrupted *.bbtmp atomic-save staging files.
    bool PrepareSaveDirectoryAndSoundFlag() const;

    // Persists launcher configuration to ApplicationData.Current.LocalSettings (with INI mirror
    // in LocalFolder/bbport_uwp.ini so the C runtime can also read settings synchronously).
    void SaveSettings(const LauncherPersistentSettings& settings);
    LauncherPersistentSettings LoadSettings() const;

private:
    StorageLayer() = default;
    StorageRoots roots_{};
    bool initialized_ = false;
};

} // namespace bbport::uwp

#ifdef __cplusplus
extern "C" {
#endif
int bbport_uwp_storage_get_roots(char* app0_out, size_t app0_cap,
                                 char* user_out, size_t user_cap,
                                 char* temp_out, size_t temp_cap);
#ifdef __cplusplus
}
#endif
