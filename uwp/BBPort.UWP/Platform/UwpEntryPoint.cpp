// SPDX-License-Identifier: GPL-2.0-or-later
// UwpEntryPoint: Xbox Hardware Rating Probe + CoreWindow / Game Mode Entry Bridge for bbport.
// Connects the UWP launcher / CoreWindow swapchain surface to src/probe.c (bbport_uwp_run_probe)
// and uwp/renderer/d3d12/BbgpuUwpBridge.cpp.

#include "LifecycleManager.h"
#include "XAudio2Sink.h"
#if __has_include("GamepadInput.h")
#include "GamepadInput.h"
#define BBPORT_HAS_GAMEPAD_INPUT 1
#endif
#if __has_include("StorageLayer.h")
#include "StorageLayer.h"
#define BBPORT_HAS_STORAGE_LAYER 1
#endif
#if __has_include("UwpFileLoader.h")
#include "UwpFileLoader.h"
#define BBPORT_HAS_FILE_LOADER 1
#endif
#include "../../../gpu/bbgpu.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>
#endif

extern "C" {
int bbport_uwp_run_probe(int argc, char** argv);
int bbgpu_uwp_probe_d3d12_fl12_0(void);
}

namespace bbport::uwp {

struct XboxHardwareRating {
    std::string device_family = "Windows.Desktop";
    std::string console_tier  = "Desktop x64";
    uint64_t app_memory_limit_mb = 5120;
    bool is_xbox = false;
    bool is_game_mode = false;       // True when AppMemoryUsageLimit > 1.5 GiB & D3D12 FL 12_0 succeeds
    bool d3d12_fl12_0_available = false;
    int recommended_output_height = 1080;
    int recommended_preset = 1;      // Quality
};

XboxHardwareRating ProbeXboxHardwareRating() {
    XboxHardwareRating rating{};

#if defined(_WIN32)
    try {
        const auto family = winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily();
        rating.device_family = winrt::to_string(family);
        rating.is_xbox = (rating.device_family == "Windows.Xbox");

        const uint64_t limit_bytes = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
        rating.app_memory_limit_mb = limit_bytes / (1024ull * 1024ull);
    } catch (...) {
    }
#endif

#if defined(BBPORT_UWP) && defined(BB_GPU_UWP_ABI_READY)
    rating.d3d12_fl12_0_available = (bbgpu_uwp_probe_d3d12_fl12_0() != 0);
#endif
    // In Xbox Dev Mode App Mode (non-Game), AppMemoryUsageLimit is ~1,024 MiB and D3D12 is blocked;
    // in Game Mode (`uap3:AppExtension Name="windows.game"` / Game type in Dev Home), limit is ~5,120 MiB.
    rating.is_game_mode = (!rating.is_xbox) ||
                          (rating.d3d12_fl12_0_available && rating.app_memory_limit_mb >= 2048);

    if (rating.is_xbox) {
        rating.console_tier = rating.is_game_mode
            ? "Xbox Series X|S (Dev Mode — Game Resource Mode, 5 GiB Cap)"
            : "Xbox Console (App Mode — Switch App Type to 'Game' in Dev Home for D3D12 & 5 GiB!)";
        rating.recommended_output_height = 1080; // Safe default within 5 GiB unified budget
        rating.recommended_preset = 1;           // Quality (720p internal -> 1080p spatial/TAA)
    }
    return rating;
}

#if defined(BBPORT_HAS_STORAGE_LAYER) && defined(BBPORT_HAS_FILE_LOADER)
namespace {

void ExportLauncherEnvVars(const LauncherPersistentSettings& s, const FirstStartAudit& audit) {
    auto set_kv = [](const char* k, const std::string& v) {
#if defined(_WIN32)
        _putenv_s(k, v.c_str());
#else
        setenv(k, v.c_str(), 1);
#endif
    };

    set_kv("BB_UWP_DMEM_MB", std::to_string(s.dmem_budget_mb));
    set_kv("BB_TARGET_FPS", std::to_string(s.target_fps));
    set_kv("BB_SCENE_HEIGHT", std::to_string(s.output_height));
    set_kv("BB_SWAP_CONFIRM", s.swap_confirm_circle ? "1" : "0");
    set_kv("BB_ONLINE_ENABLE", s.enable_online_shadnet ? "1" : "0");
    if (!s.shadnet_server.empty()) set_kv("BB_SHADNET_HOST", s.shadnet_server);
    if (!s.shadnet_user.empty()) set_kv("BB_SHADNET_USER", s.shadnet_user);
    set_kv("BB_SHADER_THREADS", std::to_string(s.shader_compile_threads));
    set_kv("BB_APP0_OVERRIDE", audit.resolved_app0.generic_string());
}

} // namespace
#endif

// Launches the native Bloodborne runtime in-process once LauncherPage validates FirstStartAudit.
int LaunchBloodborneInProcess(void* core_window_abi, uint32_t width, uint32_t height) {
#if defined(BBPORT_HAS_STORAGE_LAYER) && defined(BBPORT_HAS_FILE_LOADER)
    auto& storage = StorageLayer::Instance();
    if (!storage.Initialize()) {
        return -10;
    }
    storage.PrepareSaveDirectoryAndSoundFlag();

    const FirstStartAudit audit = UwpFileLoader::RunFirstStartCheck(storage.Roots().game_app0_dir);
    if (!audit.ReadyToLaunch()) {
        return -11;
    }

    (void)ProbeXboxHardwareRating();
    const LauncherPersistentSettings settings = storage.LoadSettings();
#if defined(BBPORT_HAS_GAMEPAD_INPUT)
    GamepadInput::Instance().Initialize();
    GamepadProfile pad_prof = GamepadInput::Instance().GetProfile();
    pad_prof.swap_confirm_circle = settings.swap_confirm_circle;
    GamepadInput::Instance().SetProfile(pad_prof);
#endif
    LifecycleManager::Instance().Initialize();

    ExportLauncherEnvVars(settings, audit);
#if defined(BBPORT_UWP) && defined(BB_GPU_UWP_ABI_READY)
    bbgpu_uwp_set_core_window(core_window_abi, static_cast<int32_t>(width), static_cast<int32_t>(height));
#else
    (void)core_window_abi;
    (void)width;
    (void)height;
#endif

    const std::string boot_img = audit.boot_linked_present
        ? (audit.resolved_app0 / "boot-linked.bin").generic_string()
        : (audit.resolved_app0 / "eboot.bin").generic_string();
    const std::string app0_dir = audit.resolved_app0.generic_string();

    std::string arg0 = "BBPort.UWP.exe";
    std::string arg1 = boot_img;
    std::string arg2 = app0_dir;
    char* argv_vec[] = {arg0.data(), arg1.data(), arg2.data(), nullptr};

    const int rc = bbport_uwp_run_probe(3, argv_vec);

    XAudio2Sink::Instance().Shutdown();
    LifecycleManager::Instance().Shutdown();
#if defined(BBPORT_HAS_GAMEPAD_INPUT)
    GamepadInput::Instance().Shutdown();
#endif
    return rc;
#else
    (void)core_window_abi;
    (void)width;
    (void)height;
    return -12;
#endif
}

} // namespace bbport::uwp

extern "C" int LaunchBloodborneFromXaml(void* core_window_abi, uint32_t width, uint32_t height) {
    return bbport::uwp::LaunchBloodborneInProcess(core_window_abi, width, height);
}

#if defined(_WIN32) && defined(BBPORT_UWP_COREWINDOW_ONLY)
struct BbPortCoreView : winrt::implements<BbPortCoreView,
                                          winrt::Windows::ApplicationModel::Core::IFrameworkViewSource,
                                          winrt::Windows::ApplicationModel::Core::IFrameworkView> {
    winrt::Windows::ApplicationModel::Core::IFrameworkView CreateView() { return *this; }
    void Initialize(const winrt::Windows::ApplicationModel::Core::CoreApplicationView&) {}
    void Load(const winrt::hstring&) {}
    void Uninitialize() {}

    void SetWindow(const winrt::Windows::UI::Core::CoreWindow& window) {
        window_ = window;
    }

    void Run() {
        winrt::Windows::UI::ViewManagement::ApplicationView::GetForCurrentView().TryEnterFullScreenMode();
        window_.Activate();
        const auto bounds = window_.Bounds();
        const uint32_t w = bounds.Width > 0 ? static_cast<uint32_t>(bounds.Width) : 1920u;
        const uint32_t h = bounds.Height > 0 ? static_cast<uint32_t>(bounds.Height) : 1080u;
        bbport::uwp::LaunchBloodborneInProcess(winrt::get_abi(window_), w, h);
    }

private:
    winrt::Windows::UI::Core::CoreWindow window_{nullptr};
};

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    winrt::init_apartment();
    winrt::Windows::ApplicationModel::Core::CoreApplication::Run(winrt::make<BbPortCoreView>());
    return 0;
}
#endif
