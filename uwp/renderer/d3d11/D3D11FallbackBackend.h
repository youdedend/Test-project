// SPDX-License-Identifier: GPL-2.0-or-later
// D3D11FallbackBackend: Direct3D 11 Feature Level 10_0 safe-boot presenter for Xbox Dev Mode.
//
// Why this exists:
//   In Xbox Dev Mode, Direct3D 12 (FL 12_0) is ONLY available when the UWP package is registered
//   under "Game" resource mode in Dev Home. In default "App" resource mode, D3D12CreateDevice fails
//   and only Direct3D 11 Feature Level 10_0 is guaranteed. This fallback prevents startup crashes
//   and renders a clear visual status banner instructing the user to switch the package to Game Mode.
#pragma once

#include <cstdint>

namespace bbport::uwp::renderer::d3d11 {

class D3D11FallbackBackend {
public:
    D3D11FallbackBackend();
    ~D3D11FallbackBackend();

    bool Initialize(void* core_window_unknown, uint32_t width, uint32_t height);
    void Shutdown();

    bool RenderDiagnosticFrame(const float clear_rgba[4]);
    void OnSuspending();
    void OnResuming();

    bool IsInitialized() const { return initialized_; }

private:
    bool initialized_ = false;
    uint32_t width_ = 1920;
    uint32_t height_ = 1080;

#if defined(_WIN32)
    struct NativeState;
    NativeState* native_ = nullptr;
#endif
};

} // namespace bbport::uwp::renderer::d3d11
