// SPDX-License-Identifier: GPL-2.0-or-later
#include "D3D12DescriptorManager.h"

#include <algorithm>

#if defined(_WIN32)
#include <d3d12.h>
#endif

namespace bbport::uwp::renderer::d3d12 {

namespace {

uint64_t Mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

} // namespace

uint64_t TextureSetKey::ComputeHash(const uint64_t* ids,
                                    uint32_t count,
                                    uint32_t registry_gen,
                                    uint32_t scene_gen) {
    const uint32_t clamped = std::min<uint32_t>(count, static_cast<uint32_t>(kMaxTextures));
    uint64_t seed = Mix64((static_cast<uint64_t>(registry_gen) << 32) ^
                          static_cast<uint64_t>(scene_gen) ^
                          (static_cast<uint64_t>(clamped) * 0x9e3779b97f4a7c15ULL));
    for (uint32_t i = 0; i < clamped; ++i) {
        seed ^= Mix64(ids[i] + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
    }
    return seed == 0 ? 1ULL : seed;
}

D3D12DescriptorManager::D3D12DescriptorManager(uint32_t srv_capacity,
                                               uint64_t max_memo_backing_bytes)
    : srv_capacity_(srv_capacity > 0 ? srv_capacity : kDefaultSrvHeapCapacity),
      max_memo_backing_bytes_(max_memo_backing_bytes) {}

D3D12DescriptorManager::~D3D12DescriptorManager() {
    Shutdown();
}

bool D3D12DescriptorManager::Initialize(void* d3d12_device_unknown) {
    std::lock_guard<std::mutex> lock(mutex_);
    memo_map_.clear();
    lru_order_.clear();
    free_ranges_.clear();
    next_linear_slot_ = 0;
    current_backing_bytes_ = 0;

#if defined(_WIN32)
    if (d3d12_device_unknown) {
        auto* dev = static_cast<ID3D12Device*>(d3d12_device_unknown);

        D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
        srv_desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_desc.NumDescriptors = srv_capacity_;
        srv_desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ID3D12DescriptorHeap* srv_heap = nullptr;
        if (FAILED(dev->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&srv_heap)))) {
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC samp_desc{};
        samp_desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        samp_desc.NumDescriptors = kDefaultSamplerHeapCapacity;
        samp_desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ID3D12DescriptorHeap* samp_heap = nullptr;
        if (FAILED(dev->CreateDescriptorHeap(&samp_desc, IID_PPV_ARGS(&samp_heap)))) {
            srv_heap->Release();
            return false;
        }

        native_srv_heap_     = srv_heap;
        native_sampler_heap_ = samp_heap;
    }
#else
    (void)d3d12_device_unknown;
#endif
    return true;
}

void D3D12DescriptorManager::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
#if defined(_WIN32)
    if (native_sampler_heap_) {
        static_cast<ID3D12DescriptorHeap*>(native_sampler_heap_)->Release();
        native_sampler_heap_ = nullptr;
    }
    if (native_srv_heap_) {
        static_cast<ID3D12DescriptorHeap*>(native_srv_heap_)->Release();
        native_srv_heap_ = nullptr;
    }
#endif
    memo_map_.clear();
    lru_order_.clear();
    free_ranges_.clear();
    next_linear_slot_ = 0;
    current_backing_bytes_ = 0;
}

bool D3D12DescriptorManager::AllocateContiguousLocked(uint32_t count, uint32_t* out_base) {
    if (count == 0 || !out_base) return false;
    for (auto it = free_ranges_.begin(); it != free_ranges_.end(); ++it) {
        if (it->second >= count) {
            *out_base = it->first;
            if (it->second == count) {
                free_ranges_.erase(it);
            } else {
                it->first += count;
                it->second -= count;
            }
            return true;
        }
    }
    if (next_linear_slot_ + count <= srv_capacity_) {
        *out_base = next_linear_slot_;
        next_linear_slot_ += count;
        return true;
    }
    return false;
}

void D3D12DescriptorManager::FreeContiguousLocked(uint32_t base, uint32_t count) {
    if (count == 0) return;
    free_ranges_.emplace_back(base, count);
    std::sort(free_ranges_.begin(), free_ranges_.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::pair<uint32_t, uint32_t>> merged;
    merged.reserve(free_ranges_.size());
    for (const auto& r : free_ranges_) {
        if (!merged.empty() && merged.back().first + merged.back().second == r.first) {
            merged.back().second += r.second;
        } else {
            merged.push_back(r);
        }
    }
    free_ranges_.swap(merged);
}

DescriptorSlice D3D12DescriptorManager::AcquireTextureSetSlice(const TextureSetKey& raw_key,
                                                               uint64_t current_frame,
                                                               uint64_t backing_bytes_estimate,
                                                               bool* out_cache_hit) {
    TextureSetKey key = raw_key;
    if (key.precomputed_hash == 0) {
        key.precomputed_hash = TextureSetKey::ComputeHash(
            key.image_view_ids, key.count, key.registry_generation, key.scene_targets_generation);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = memo_map_.find(key.precomputed_hash); it != memo_map_.end()) {
        // Verify generation tags match to prevent hash collisions across scene resets.
        if (it->second.key.registry_generation == key.registry_generation &&
            it->second.key.scene_targets_generation == key.scene_targets_generation &&
            it->second.key.count == key.count) {
            lru_order_.erase(it->second.lru_it);
            lru_order_.push_front(key.precomputed_hash);
            it->second.lru_it = lru_order_.begin();
            it->second.slice.last_used_frame = current_frame;
            if (out_cache_hit) *out_cache_hit = true;
            return it->second.slice;
        }
        // Stale collision: recycle slice before reallocating.
        FreeContiguousLocked(it->second.slice.heap_base_index, it->second.slice.descriptor_count);
        if (current_backing_bytes_ >= it->second.slice.estimated_backing_bytes) {
            current_backing_bytes_ -= it->second.slice.estimated_backing_bytes;
        } else {
            current_backing_bytes_ = 0;
        }
        lru_order_.erase(it->second.lru_it);
        memo_map_.erase(it);
    }

    if (out_cache_hit) *out_cache_hit = false;
    const uint32_t needed_slots = std::max<uint32_t>(1u, std::min<uint32_t>(key.count, 16u));

    // Enforce the Xbox 5 GiB memoization budget before allocating a fresh descriptor table.
    while (!lru_order_.empty() &&
           max_memo_backing_bytes_ > 0 &&
           current_backing_bytes_ + backing_bytes_estimate > max_memo_backing_bytes_) {
        const uint64_t victim_hash = lru_order_.back();
        auto vit = memo_map_.find(victim_hash);
        if (vit == memo_map_.end()) {
            lru_order_.pop_back();
            continue;
        }
        if (vit->second.slice.last_used_frame == current_frame) {
            break; // Never evict descriptors bound in the active in-flight command list!
        }
        FreeContiguousLocked(vit->second.slice.heap_base_index, vit->second.slice.descriptor_count);
        if (current_backing_bytes_ >= vit->second.slice.estimated_backing_bytes) {
            current_backing_bytes_ -= vit->second.slice.estimated_backing_bytes;
        } else {
            current_backing_bytes_ = 0;
        }
        lru_order_.pop_back();
        memo_map_.erase(vit);
    }

    uint32_t base_slot = 0;
    if (!AllocateContiguousLocked(needed_slots, &base_slot)) {
        // Heap fragmented/full: evict oldest non-current-frame slice and retry once.
        while (!lru_order_.empty()) {
            const uint64_t victim_hash = lru_order_.back();
            auto vit = memo_map_.find(victim_hash);
            if (vit == memo_map_.end()) {
                lru_order_.pop_back();
                continue;
            }
            if (vit->second.slice.last_used_frame == current_frame) {
                break;
            }
            FreeContiguousLocked(vit->second.slice.heap_base_index, vit->second.slice.descriptor_count);
            if (current_backing_bytes_ >= vit->second.slice.estimated_backing_bytes) {
                current_backing_bytes_ -= vit->second.slice.estimated_backing_bytes;
            }
            lru_order_.pop_back();
            memo_map_.erase(vit);
            if (AllocateContiguousLocked(needed_slots, &base_slot)) {
                break;
            }
        }
        if (base_slot == 0 && next_linear_slot_ > 0 && memo_map_.size() > 0 &&
            memo_map_.begin()->second.slice.heap_base_index == 0) {
            return DescriptorSlice{};
        }
    }

    // TODO(port): Copy Liverpool T# SRV CPU handles (`ID3D12Device::CopyDescriptorsSimple`)
    // from TextureCache's CPU staging SRV heap into `native_srv_heap_ + base_slot`.
    DescriptorSlice slice{};
    slice.heap_base_index         = base_slot;
    slice.descriptor_count        = needed_slots;
    slice.last_used_frame         = current_frame;
    slice.estimated_backing_bytes = backing_bytes_estimate;
    slice.valid                   = true;

    lru_order_.push_front(key.precomputed_hash);
    MemoEntry entry{};
    entry.key    = key;
    entry.slice  = slice;
    entry.lru_it = lru_order_.begin();
    memo_map_.emplace(key.precomputed_hash, std::move(entry));
    current_backing_bytes_ += backing_bytes_estimate;

    return slice;
}

size_t D3D12DescriptorManager::InvalidateStaleGenerations(uint32_t min_registry_gen,
                                                          uint32_t min_scene_gen) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t removed = 0;
    for (auto it = memo_map_.begin(); it != memo_map_.end();) {
        if (it->second.key.registry_generation < min_registry_gen ||
            it->second.key.scene_targets_generation < min_scene_gen) {
            FreeContiguousLocked(it->second.slice.heap_base_index, it->second.slice.descriptor_count);
            if (current_backing_bytes_ >= it->second.slice.estimated_backing_bytes) {
                current_backing_bytes_ -= it->second.slice.estimated_backing_bytes;
            } else {
                current_backing_bytes_ = 0;
            }
            lru_order_.erase(it->second.lru_it);
            it = memo_map_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

uint64_t D3D12DescriptorManager::EvictColdEntries(uint64_t target_bytes_to_free,
                                                  uint64_t current_frame,
                                                  uint64_t protect_recent_frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t freed_bytes = 0;

    auto rev_it = lru_order_.end();
    while (rev_it != lru_order_.begin() && freed_bytes < target_bytes_to_free) {
        --rev_it;
        const uint64_t hash = *rev_it;
        auto mit = memo_map_.find(hash);
        if (mit == memo_map_.end()) {
            rev_it = lru_order_.erase(rev_it);
            continue;
        }
        const uint64_t age = (current_frame >= mit->second.slice.last_used_frame)
            ? (current_frame - mit->second.slice.last_used_frame)
            : 0;
        if (age < protect_recent_frames) {
            continue; // Still potentially referenced by triple-buffered in-flight GPU frames
        }

        FreeContiguousLocked(mit->second.slice.heap_base_index, mit->second.slice.descriptor_count);
        const uint64_t est = mit->second.slice.estimated_backing_bytes;
        freed_bytes += est;
        if (current_backing_bytes_ >= est) {
            current_backing_bytes_ -= est;
        } else {
            current_backing_bytes_ = 0;
        }
        rev_it = lru_order_.erase(rev_it);
        memo_map_.erase(mit);
    }

    return freed_bytes;
}

size_t D3D12DescriptorManager::CachedSetCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return memo_map_.size();
}

uint64_t D3D12DescriptorManager::TrackedBackingBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_backing_bytes_;
}

uint32_t D3D12DescriptorManager::UsedDescriptors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t free_sum = 0;
    for (const auto& r : free_ranges_) free_sum += r.second;
    return (next_linear_slot_ >= free_sum) ? (next_linear_slot_ - free_sum) : 0;
}

} // namespace bbport::uwp::renderer::d3d12
