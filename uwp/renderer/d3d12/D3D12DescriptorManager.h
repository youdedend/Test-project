// SPDX-License-Identifier: GPL-2.0-or-later
// D3D12DescriptorManager: translates shadPS4's TextureSetMemo (vk_rasterizer.h) to D3D12
// shader-visible CBV_SRV_UAV + Sampler heaps with Xbox 5 GiB LRU memory-pressure eviction.
#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace bbport::uwp::renderer::d3d12 {

// Mirrors gpu/shadps4/src/video_core/renderer_vulkan/vk_rasterizer.h TextureSetMemo key inputs:
// <= 16 Liverpool T# image view bindings + registry generation + scene-target generation.
struct TextureSetKey {
    static constexpr size_t kMaxTextures = 16;
    uint64_t image_view_ids[kMaxTextures]{};
    uint32_t count = 0;
    uint32_t registry_generation = 0;
    uint32_t scene_targets_generation = 0;
    uint64_t precomputed_hash = 0;

    static uint64_t ComputeHash(const uint64_t* ids,
                                uint32_t count,
                                uint32_t registry_gen,
                                uint32_t scene_gen);
};

struct DescriptorSlice {
    uint32_t heap_base_index = 0;
    uint32_t descriptor_count = 0;
    uint64_t last_used_frame = 0;
    uint64_t estimated_backing_bytes = 0;
    bool valid = false;
};

class D3D12DescriptorManager {
public:
    // D3D12 Tier 1/2 FL 12_0 limits on Xbox Dev Mode:
    // - Max 1,000,000 CBV_SRV_UAV descriptors per shader-visible heap (we budget 65,536 -> ~2 MiB)
    // - Max 2,048 Samplers per shader-visible sampler heap (strict D3D12 spec ceiling!)
    static constexpr uint32_t kDefaultSrvHeapCapacity     = 65536;
    static constexpr uint32_t kDefaultSamplerHeapCapacity = 2048;

    explicit D3D12DescriptorManager(uint32_t srv_capacity = kDefaultSrvHeapCapacity,
                                    uint64_t max_memo_backing_bytes = 384ull * 1024ull * 1024ull);
    ~D3D12DescriptorManager();

    bool Initialize(void* d3d12_device_unknown);
    void Shutdown();

    // Looks up or allocates a contiguous SRV table slice for a Liverpool T# texture set.
    DescriptorSlice AcquireTextureSetSlice(const TextureSetKey& key,
                                           uint64_t current_frame,
                                           uint64_t backing_bytes_estimate,
                                           bool* out_cache_hit = nullptr);

    // Invalidates stale entries when TextureCache::RegistryGeneration or SceneTargets::Generation bumps.
    size_t InvalidateStaleGenerations(uint32_t min_registry_gen, uint32_t min_scene_gen);

    // LRU memory-pressure eviction called by bbgpu_uwp_trim_memory() / LifecycleManager.
    // Frees cold memoized texture-set slices not used within `protect_recent_frames` until at least
    // `target_bytes_to_free` have been reclaimed.
    uint64_t EvictColdEntries(uint64_t target_bytes_to_free,
                              uint64_t current_frame,
                              uint64_t protect_recent_frames = 3);

    size_t CachedSetCount() const;
    uint64_t TrackedBackingBytes() const;
    uint32_t UsedDescriptors() const;
    void* NativeSrvHeap() const { return native_srv_heap_; }
    void* NativeSamplerHeap() const { return native_sampler_heap_; }

private:
    struct MemoEntry {
        TextureSetKey key{};
        DescriptorSlice slice{};
        std::list<uint64_t>::iterator lru_it;
    };

    mutable std::mutex mutex_;
    uint32_t srv_capacity_ = kDefaultSrvHeapCapacity;
    uint32_t next_linear_slot_ = 0;
    uint64_t max_memo_backing_bytes_ = 0;
    uint64_t current_backing_bytes_ = 0;

    std::vector<std::pair<uint32_t, uint32_t>> free_ranges_; // (base, count)
    std::list<uint64_t> lru_order_;                          // front = MRU, back = LRU
    std::unordered_map<uint64_t, MemoEntry> memo_map_;

    void* native_srv_heap_ = nullptr;
    void* native_sampler_heap_ = nullptr;

    bool AllocateContiguousLocked(uint32_t count, uint32_t* out_base);
    void FreeContiguousLocked(uint32_t base, uint32_t count);
};

} // namespace bbport::uwp::renderer::d3d12
