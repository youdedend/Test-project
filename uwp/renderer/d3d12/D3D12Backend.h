// SPDX-License-Identifier: GPL-2.0-or-later
// D3D12Backend: Xbox Dev Mode Game Mode Direct3D 12 (Feature Level 12_0) device, CoreWindow
// flip-model swapchain, root signature, frame-fence synchronization, and shadPS4 translation hooks.
//
// Xbox Dev Mode Ground Truth Enforced Here:
//   - Requests D3D_FEATURE_LEVEL_12_0 (never 12_1 or 12_2, which fail on Xbox One / Dev Mode caps).
//   - Requires Game Resource Mode; if D3D12CreateDevice fails in App Mode, caller falls back cleanly
//     to D3D11FallbackBackend (Feature Level 10_0).
//   - Guards DXGI_PRESENT_ALLOW_TEARING behind explicit VSync-off check (avoids dusklight-uwp tearing).
#pragma once

#include "D3D12DescriptorManager.h"
#include "D3D12RenderPassCache.h"
#include "SpirvToDxilCompiler.h"

#include <cstdint>
#include <mutex>

namespace bbport::uwp::renderer::d3d12 {

struct D3D12InitParams {
    void* core_window_unknown = nullptr; // ABI::Windows::UI::Core::ICoreWindow*
    uint32_t width = 1920;
    uint32_t height = 1080;
    int render_preset = 1;               // Quality (1280x720 -> 1920x1080)
    bool vsync_enabled = true;
    uint32_t shader_threads = 2;
};

class D3D12Backend {
public:
    static constexpr uint32_t kFrameCount = 3; // Triple-buffered flip-discard swapchain

    D3D12Backend();
    ~D3D12Backend();

    // Non-destructive probe verifying whether D3D12 Feature Level 12_0 is permitted in this mode.
    static bool ProbeFeatureLevel12_0();

    bool Initialize(const D3D12InitParams& params);
    void Shutdown();

    bool Resize(uint32_t width, uint32_t height);
    bool RenderFrame(const float clear_rgba[4]);

    // PLM Suspend / Resume & Xbox 5 GiB memory-pressure hooks.
    void OnSuspending();
    void OnResuming();
    uint64_t TrimMemory(uint64_t target_bytes_to_free);

    bool IsInitialized() const { return initialized_; }
    uint64_t FrameIndex() const { return total_frames_; }
    D3D12DescriptorManager& DescriptorManager() { return descriptors_; }
    D3D12RenderPassCache& RenderPassCache() { return render_passes_; }
    const SceneResolutionScale& SceneScale() const { return scene_scale_; }

private:
    bool initialized_ = false;
    bool vsync_enabled_ = true;
    bool allow_tearing_ = false;
    uint32_t width_ = 1920;
    uint32_t height_ = 1080;
    uint32_t shader_threads_ = 2;
    uint64_t total_frames_ = 0;

    SceneResolutionScale scene_scale_{};
    D3D12DescriptorManager descriptors_;
    D3D12RenderPassCache render_passes_;

#if defined(_WIN32)
    struct NativeState;
    NativeState* native_ = nullptr;
    bool CreateLiverpoolRootSignature();
    bool CreateBackbufferRenderTargets();
    void WaitForGpuIdle();
    void MoveToNextFrame();
#endif
};

} // namespace bbport::uwp::renderer::d3d12
