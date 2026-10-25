// SPDX-License-Identifier: GPL-2.0-or-later
#include "D3D12RenderPassCache.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#include <d3d12.h>
#endif

namespace bbport::uwp::renderer::d3d12 {

uint64_t RenderPassSignature::Hash() const {
    uint64_t h = 14695981039346656037ULL;
    const auto* bytes = reinterpret_cast<const uint8_t*>(this);
    for (size_t i = 0; i < sizeof(RenderPassSignature); ++i) {
        h ^= bytes[i];
        h *= 1099511628211ULL;
    }
    return h == 0 ? 1ULL : h;
}

D3D12RenderPassCache::~D3D12RenderPassCache() {
    Clear();
}

SceneResolutionScale D3D12RenderPassCache::ComputeXboxSceneScale(uint32_t output_width,
                                                                 uint32_t output_height,
                                                                 int preset) {
    SceneResolutionScale s{};
    s.output_width  = std::max<uint32_t>(output_width, 1280u);
    s.output_height = std::max<uint32_t>(output_height, 720u);

    // Ratios mirror gpu/shadps4/src/video_core/renderer_vulkan/vk_scene_resolution.h
    uint32_t num = 2, den = 3; // Quality (66.7%, e.g. 1280x720 -> 1920x1080)
    switch (preset) {
        case 0: num = 1;  den = 1;  s.mip_lod_bias_q8 = 0;    s.half_res_proxies_enabled = false; break;
        case 1: num = 2;  den = 3;  s.mip_lod_bias_q8 = -149; s.half_res_proxies_enabled = true;  break;
        case 2: num = 58; den = 100; s.mip_lod_bias_q8 = -201; s.half_res_proxies_enabled = true; break;
        default: num = 1; den = 2;  s.mip_lod_bias_q8 = -256; s.half_res_proxies_enabled = true;  break;
    }

    s.internal_width  = ((s.output_width * num / den) + 1u) & ~1u;
    s.internal_height = ((s.output_height * num / den) + 1u) & ~1u;
    return s;
}

void* D3D12RenderPassCache::LookupOrCreateGraphicsPso(void* d3d12_device_unknown,
                                                      void* root_signature_unknown,
                                                      const RenderPassSignature& sig,
                                                      const void* vs_dxil, size_t vs_size,
                                                      const void* ps_dxil, size_t ps_size,
                                                      bool* out_cache_hit) {
    const uint64_t key = sig.Hash();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = pso_cache_.find(key); it != pso_cache_.end()) {
            if (out_cache_hit) *out_cache_hit = true;
            return it->second;
        }
    }
    if (out_cache_hit) *out_cache_hit = false;

#if defined(_WIN32)
    if (d3d12_device_unknown && root_signature_unknown && vs_dxil && vs_size > 0) {
        auto* dev  = static_cast<ID3D12Device*>(d3d12_device_unknown);
        auto* root = static_cast<ID3D12RootSignature*>(root_signature_unknown);

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root;
        desc.VS             = {vs_dxil, vs_size};
        if (ps_dxil && ps_size > 0) {
            desc.PS = {ps_dxil, ps_size};
        }
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable       = TRUE;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.DepthStencilState.DepthEnable         = (sig.depth_dxgi_format != 0) ? TRUE : FALSE;
        desc.DepthStencilState.DepthWriteMask      = D3D12_DEPTH_WRITE_MASK_ALL;
        desc.DepthStencilState.DepthFunc           = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        desc.PrimitiveTopologyType                 = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets                      = std::min<uint32_t>(sig.num_color_attachments, 8u);
        for (uint32_t i = 0; i < desc.NumRenderTargets; ++i) {
            desc.RTVFormats[i] = static_cast<DXGI_FORMAT>(sig.color_dxgi_formats[i]);
        }
        desc.DSVFormat          = static_cast<DXGI_FORMAT>(sig.depth_dxgi_format);
        desc.SampleDesc.Count   = std::max<uint32_t>(sig.sample_count, 1u);

        ID3D12PipelineState* pso = nullptr;
        if (SUCCEEDED(dev->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso))) && pso) {
            std::lock_guard<std::mutex> lock(mutex_);
            pso_cache_[key] = pso;
            return pso;
        }
    }
#else
    (void)d3d12_device_unknown;
    (void)root_signature_unknown;
    (void)vs_dxil;
    (void)vs_size;
    (void)ps_dxil;
    (void)ps_size;
#endif
    return nullptr;
}

void D3D12RenderPassCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
#if defined(_WIN32)
    for (auto& kv : pso_cache_) {
        if (kv.second) {
            static_cast<ID3D12PipelineState*>(kv.second)->Release();
        }
    }
#endif
    pso_cache_.clear();
}

size_t D3D12RenderPassCache::CachedPsoCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pso_cache_.size();
}

} // namespace bbport::uwp::renderer::d3d12
