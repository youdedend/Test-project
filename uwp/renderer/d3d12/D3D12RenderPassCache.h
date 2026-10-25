// SPDX-License-Identifier: GPL-2.0-or-later
// D3D12RenderPassCache: translates shadPS4's RenderStateMemo (BeginSignature) & SceneProxies
// (vk_scene_resolution.h) to D3D12 PSO memoization + BeginRenderPass / OMSetRenderTargets.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace bbport::uwp::renderer::d3d12 {

// Mirrors gpu/shadps4/src/video_core/renderer_vulkan/vk_rasterizer.h BeginSignature
struct RenderPassSignature {
    uint32_t color_dxgi_formats[8]{};
    uint32_t num_color_attachments = 0;
    uint32_t depth_dxgi_format = 0;      // e.g. DXGI_FORMAT_D32_FLOAT_S8X24_UINT (20)
    uint32_t sample_count = 1;
    uint32_t primitive_topology = 4;     // D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
    uint32_t blend_state_mask = 0;
    uint32_t depth_stencil_bits = 0;
    uint64_t vs_shader_hash = 0;
    uint64_t ps_shader_hash = 0;

    uint64_t Hash() const;
};

struct SceneResolutionScale {
    uint32_t internal_width = 1280;
    uint32_t internal_height = 720;
    uint32_t output_width = 1920;
    uint32_t output_height = 1080;
    int32_t  mip_lod_bias_q8 = -128; // -0.5f LOD bias matching SceneMipBias in bbport_toggles.h
    bool     half_res_proxies_enabled = true;
};

class D3D12RenderPassCache {
public:
    D3D12RenderPassCache() = default;
    ~D3D12RenderPassCache();

    // Preset: 0 = NativeAA (100%), 1 = Quality (67%), 2 = Balanced (58%), 3 = Performance (50%)
    static SceneResolutionScale ComputeXboxSceneScale(uint32_t output_width,
                                                      uint32_t output_height,
                                                      int preset);

    // Memoizes D3D12 Graphics Pipeline State Objects keyed by RenderPassSignature::Hash().
    void* LookupOrCreateGraphicsPso(void* d3d12_device_unknown,
                                    void* root_signature_unknown,
                                    const RenderPassSignature& sig,
                                    const void* vs_dxil, size_t vs_size,
                                    const void* ps_dxil, size_t ps_size,
                                    bool* out_cache_hit = nullptr);

    void Clear();
    size_t CachedPsoCount() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, void*> pso_cache_;
};

} // namespace bbport::uwp::renderer::d3d12
