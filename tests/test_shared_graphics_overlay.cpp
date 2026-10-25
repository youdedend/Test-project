// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the actual overlay widgets with Dear ImGui's navigation activation.
// Including the implementation keeps internal menu helpers out of the public API.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include "imgui_internal.h"
#define runtime_restart overlay_test_restart
#include "../gpu/shim/bbport_overlay.cpp"
#undef runtime_restart
#include <windows.h>

static int restarts = 0;
// Keep the restart observable while linking the renderer's actual runtime dependencies.
extern "C" void overlay_test_restart() { ++restarts; }

static std::string Frame(ImGuiID activate = 0, ImGuiWindow* target = nullptr) {
    ImGui::GetIO().DeltaTime = 1.0f / 60;
    ImGui::NewFrame();
    auto& g = *ImGui::GetCurrentContext();
    if (activate) {
        g.NavWindow = target;
        g.NavInputSource = ImGuiInputSource_Keyboard;
        g.NavActivateId = g.NavActivateDownId = g.NavActivatePressedId = activate;
        g.NavActivateFlags = ImGuiActivateFlags_None;
    }
    ImGui::LogToBuffer();
    BbOverlay::Menu();
    const std::string text = g.LogBuffer.c_str();
    ImGui::LogFinish();
    ImGui::Render();
    return text;
}

int main() {
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    wchar_t temporary[MAX_PATH]{};
    assert(GetTempFileNameW(std::filesystem::temp_directory_path().c_str(), L"bbo", 0, temporary));
    const auto path = std::filesystem::path(temporary).string();
    _putenv_s("BB_CONFIG", path.c_str());
    _putenv_s("BB_SHARED_GRAPHICS_CONFIG", "1");
    std::ofstream(path) << "upscaler=fsr411\nobject_motion=1\nfsr411_backend=vk\nobject_motion_cache=0\n";
    BbSettings::Load();
    auto& s = BbSettings::Get();
    s.fsr411_supported = s.fsr4_supported = true;
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {3840, 2160};
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    assert(pixels && width > 0 && height > 0);
    BbOverlay::SetOpen(true);
    Frame();
    auto text = Frame();
    assert(text.find("FSR 4.1.1 backend") != std::string::npos);
    assert(text.find("Motion-vector cache (experimental)") != std::string::npos);
    ImGuiWindow* window = ImGui::FindWindowByName("Bloodborne - Settings (Insert / L3+R3)");
    assert(window);
    const ImGuiID method = window->GetID("FSR 4.1.1 backend");
    const ImGuiID cache = window->GetID("Motion-vector cache (experimental)");
    Frame(method, window);
    auto& g = *ImGui::GetCurrentContext();
    assert(!g.OpenPopupStack.empty() && g.OpenPopupStack.back().Window);
    ImGuiWindow* popup = g.OpenPopupStack.back().Window;
    Frame(popup->GetID("AMD DX12"), popup);
    assert(s.fsr411_backend == BbSettings::Fsr411Dx12);
    assert(s.startup_fsr411_backend == BbSettings::Fsr411Vulkan);
    assert(BbSettings::FsrSettingsNeedRestart());
    Frame(cache, window);
    assert(s.object_motion_cache);
    text = Frame();
    assert(text.find("Active method: Vulkan (fsr4vk)") != std::string::npos);
    assert(text.find("Apply and restart") != std::string::npos);
    for (int upscaler : {BbSettings::UpscalerTaa, BbSettings::UpscalerDlss, BbSettings::UpscalerOff}) {
        s.upscaler = upscaler;
        Frame(cache, window);
        assert(s.object_motion_cache); // disabled widgets ignore activation
        Frame(method, window);
        assert(g.OpenPopupStack.empty());
        assert(s.fsr411_backend == BbSettings::Fsr411Dx12);
    }
    s.upscaler = BbSettings::UpscalerFsr411;
    s.object_motion = false;
    Frame(cache, window);
    assert(s.object_motion_cache);
    s.object_motion = true;
    for (int upscaler : {BbSettings::UpscalerFsr3, BbSettings::UpscalerFsr4, BbSettings::UpscalerFsr411}) {
        s.upscaler = upscaler;
        Frame(cache, window);
        assert(!s.object_motion_cache);
        Frame();
        Frame(cache, window);
        assert(s.object_motion_cache);
        Frame();
    }
    Frame(window->GetID("Apply and restart"), window);
    assert(restarts == 1);
    // This is what the next process reads; the running provider was not changed in place.
    BbSettings::Load();
    assert(s.fsr411_backend == BbSettings::Fsr411Dx12 && s.object_motion_cache);
    assert(!BbSettings::FsrSettingsNeedRestart());
    text = Frame();
    assert(text.find("Apply and restart") == std::string::npos);
    // Saved ML-FG selection remains visible while the session runs its FSR 3.1 fallback.
    BbSettings::SelectFrameGen(BbSettings::FrameGenFsr401);
    BbSettings::FrameGenFailure(BbSettings::FrameGenFsr401, "ML-FG file missing: test.dll");
    text = Frame();
    assert(text.find("ML-FG file missing: test.dll") != std::string::npos);
    assert(text.find("FSR 3.1 FG active") != std::string::npos);
    assert(s.frame_gen == BbSettings::FrameGenFsr401);
    s.frame_gen_hdr = true;
    text = Frame();
    assert(text.find("ML-FG paused: HDR output.") != std::string::npos);
    assert(text.find("FSR 3.1 FG active") == std::string::npos);
    BbSettings::Save();
    BbSettings::Load();
    assert(s.frame_gen == BbSettings::FrameGenFsr401 && !s.frame_gen_hdr);
    BbOverlay::SetOpen(false);
    ImGui::DestroyContext();
    std::remove(path.c_str());
    std::puts("Overlay widgets: method selection, cache gating, save and restart activation PASS");
}
