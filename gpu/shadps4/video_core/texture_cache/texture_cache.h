// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include "bbport_toggles.h"

#include <condition_variable>
#include <mutex>
#include <source_location>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>
#include <boost/container/small_vector.hpp>
#include <queue>
#include <tsl/robin_map.h>

#include "common/lru_cache.h"
#include "common/slot_vector.h"
#include "shader_recompiler/resource.h"
#include "video_core/multi_level_page_table.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/sampler.h"
#include "video_core/texture_cache/tile_manager.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {
class Runtime;
}

namespace VideoCore {

class BufferCache;
class PageManager;

class TextureCache {
    // Default values for garbage collection
    static constexpr s64 DEFAULT_PRESSURE_GC_MEMORY = 1_GB + 512_MB;
    static constexpr s64 DEFAULT_CRITICAL_GC_MEMORY = 3_GB;
    static constexpr s64 TARGET_GC_THRESHOLD = 8_GB;

    using ImageIds = boost::container::small_vector<ImageId, 16>;

    struct Traits {
        using Entry = ImageIds;
        static constexpr size_t AddressSpaceBits = 40;
        static constexpr size_t FirstLevelBits = 10;
        static constexpr size_t PageBits = 20;
    };
    using PageTable = MultiLevelPageTable<Traits>;

public:
#ifdef BB_UWP
    vk::ImageView NullView() const {
        return *null_view;
    }
    UniqueImage null_image;
    vk::UniqueImageView null_view;
#endif
    enum class BindingType : u32 {
        Texture,
        Storage,
        RenderTarget,
        DepthTarget,
        VideoOut,
    };

    struct ImageDesc {
        ImageInfo info;
        ImageViewInfo view_info;
        BindingType type{BindingType::Texture};

        ImageDesc() = default;
        ImageDesc(const AmdGpu::Image& image, const Shader::ImageResource& desc)
            : info{image, desc}, view_info{image, desc},
              type{desc.is_written ? BindingType::Storage : BindingType::Texture} {}
        ImageDesc(const AmdGpu::ColorBuffer& buffer, AmdGpu::CbDbExtent hint)
            : info{buffer, hint}, view_info{buffer}, type{BindingType::RenderTarget} {}
        ImageDesc(const AmdGpu::DepthBuffer& buffer, AmdGpu::DepthView view,
                  AmdGpu::DepthControl ctl, VAddr htile_address, AmdGpu::CbDbExtent hint,
                  bool write_buffer = false)
            : info{buffer, view.NumSlices(), htile_address, hint, write_buffer},
              view_info{buffer, view, ctl}, type{BindingType::DepthTarget} {}
        ImageDesc(const Libraries::VideoOut::BufferAttributeGroup& group, VAddr cpu_address)
            : info{group, cpu_address}, type{BindingType::VideoOut} {}
    };

public:
    TextureCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                 Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool, BufferCache& buffer_cache,
                 PageManager& tracker);
    ~TextureCache();

    /// bbport: changes whenever an image is registered or unregistered.
    [[nodiscard]] u64 RegistryGeneration() const noexcept {
        return registry_generation.load(std::memory_order_acquire);
    }

    /// bbport: FindImage's access tick for an image found by a memoized lookup. The LRU touch
    /// FindImage also does happens in UpdateImage (FindTexture), which every binding reaches.
    void MarkFound(ImageId image_id) {
        IndexImage(image_id).tick_accessed_last = scheduler.CurrentTick();
    }

    TileManager& GetTileManager() noexcept {
        return tile_manager;
    }

    /// Invalidates any image in the logical page range.
    void InvalidateMemory(VAddr addr, size_t size);

    /// Marks an image as dirty if it exists at the provided address.
    void InvalidateMemoryFromGPU(VAddr address, size_t max_size, bool whole_range = false);

    /// Evicts any images that overlap the unmapped range.
    void UnmapMemory(VAddr cpu_addr, size_t size);

    /// Schedules a copy of pending images for download back to CPU memory.
    void ProcessDownloadImages();

    /// Retrieves the image handle of the image with the provided attributes.
    [[nodiscard]] ImageId FindImage(ImageDesc& desc, bool exact_fmt = false);

    /// Retrieves image whose address matches provided
    [[nodiscard]] ImageId FindImageFromRange(VAddr address, size_t size, bool ensure_valid = true);

    /// bbport: a FindView result remembered by the caller for the same image and view info.
    struct ViewMemo {
        ImageId image_id{};
        const void* backing = nullptr;
        ImageViewId view_id{};
    };

    /// Retrieves an image view with the properties of the specified image id.
    /// `refresh` false: an image found out of date is used as is (the texture binding helper
    /// checked it beforehand; only a guest write racing with the draw can make it so).
    [[nodiscard]] ImageView& FindTexture(ImageId image_id, const ImageDesc& desc,
                                         ViewMemo* memo = nullptr, bool refresh = true);

    /// Retrieves the render target with specified properties
    [[nodiscard]] ImageView& FindRenderTarget(ImageId image_id, const ImageDesc& desc);

    /// Retrieves the depth target with specified properties
    [[nodiscard]] ImageView& FindDepthTarget(ImageId image_id, const ImageDesc& desc);

    /// bbport: whether UpdateImage has nothing to do for this image (its fast path).
    [[nodiscard]] bool IsUpToDate(ImageId image_id) const {
        const Image& image = IndexImage(image_id);
        const u32 flags = std::atomic_ref<u32>(const_cast<u32&>(reinterpret_cast<const u32&>(image.flags)))
                              .load(std::memory_order_acquire);
        constexpr u32 Dirty = static_cast<u32>(ImageFlagBits::Dirty);
        constexpr u32 Registered = static_cast<u32>(ImageFlagBits::Registered);
        return (flags & (Dirty | Registered)) == Registered &&
               image.track_addr == image.guest_begin && image.track_addr_end == image.guest_end &&
               image.lru_touched_tick == gc_tick;
    }

    /// Updates image contents if it was modified by CPU.
    void UpdateImage(ImageId image_id) {
        // bbport: a clean image already tracked and touched in this GC period needs nothing.
        // Every texture binding comes here; the mutex (shared with the fault handlers of the
        // guest threads) was ~3% of the GPU thread. Flags are read atomically: an invalidation
        // racing with this check races the same way with the locked path.
        if (!BbToggle::Disabled(BbToggle::UpdateImageFastPath)) {
            const Image& image = IndexImage(image_id);
            const u32 flags = std::atomic_ref<u32>(const_cast<u32&>(reinterpret_cast<const u32&>(image.flags)))
                                  .load(std::memory_order_acquire);
            constexpr u32 Dirty = static_cast<u32>(ImageFlagBits::Dirty);
            constexpr u32 Registered = static_cast<u32>(ImageFlagBits::Registered);
            if ((flags & (Dirty | Registered)) == Registered &&
                image.track_addr == image.guest_begin && image.track_addr_end == image.guest_end &&
                image.lru_touched_tick == gc_tick) {
                return;
            }
        }
        std::scoped_lock lock{mutex};
        Image& image = IndexImage(image_id);
        TrackImage(image_id);
        TouchImage(image);
        RefreshImage(image);
    }

    /// Resolves overlap between existing cache image and pending merged image
    [[nodiscard]] std::tuple<ImageId, int, int> ResolveOverlap(const ImageInfo& info,
                                                               BindingType binding,
                                                               ImageId cache_img_id,
                                                               ImageId merged_image_id);

    /// Resolves depth overlap and either re-creates the image or returns existing one
    [[nodiscard]] ImageId ResolveDepthOverlap(const ImageInfo& requested_info, BindingType binding,
                                              ImageId cache_img_id);

    /// Creates a new image with provided image info and copies subresources from image_id
    [[nodiscard]] ImageId ExpandImage(const ImageInfo& info, ImageId image_id);

    /// Reuploads image contents.
    void RefreshImage(Image& image);

    /// Retrieves the sampler that matches the provided S# descriptor.
    /// extra_lod_bias: bbport, added to the S#'s bias (reduced scene rendering).
    [[nodiscard]] vk::Sampler GetSampler(const AmdGpu::Sampler& sampler,
                                         AmdGpu::BorderColorBuffer border_color_base,
                                         bool is_depth, float extra_lod_bias = 0.0f);

    /// Retrieves the image with the specified id.
    /// bbport: out of GPU memory for an image of its own (scene proxies): unused textures go at
    /// once, up to `bytes` (ReclaimMemoryFor). Not with the cache's lock held.
    u64 ReclaimMemory(u64 bytes) {
        std::scoped_lock lock{mutex};
        return ReclaimMemoryFor(0, 0, bytes);
    }

    Image* TryGetImage(ImageId id, u64 uid) {
        if (!slot_images.is_allocated(id)) return nullptr;
        auto& image = slot_images[id];
        return !uid || image.image_uid == uid ? &image : nullptr;
    }
    /// bbport: TryGetImage plus the LRU touch GetImage does, so a holder that only re-reads an id it
    /// stored earlier (the upscaler, camera motion) keeps it as fresh as the old GetImage call did.
    /// Nullptr when the slot is free or its uid no longer matches - the caller skips its work.
    [[nodiscard]] Image* TryGetImageTouched(ImageId id, u64 uid) {
        Image* image = TryGetImage(id, uid);
        if (image != nullptr) {
            TouchImage(*image);
        }
        return image;
    }
    /// bbport: false once the image's slot has been freed. SlotId is a bare index without a
    /// generation, so a stale id either trips slot_vector.h ValidateIndex ("invalid or freed slot
    /// index", which stops the GPU) or silently aliases whatever image takes the slot next. Callers
    /// that resolved an id earlier in the frame (the rasterizer's two-pass binding) must re-resolve
    /// instead of indexing with it.
    [[nodiscard]] bool IsImageAlive(ImageId id) const noexcept {
        return slot_images.is_allocated(id);
    }
    /// bbport: the direct `slot_images[...]` index, for the sites inside this class that resolved
    /// an id themselves (or that keep one over a frame boundary). GetImage/GetImageView report a
    /// freed slot, but they are not what these sites go through, so an id that dies between its
    /// resolution and its use used to reach slot_vector.h:225 with a bare "invalid or freed slot
    /// index" and no hint of the site that held it. Same report here (id, caller's file:line,
    /// last frees); the assertion itself is untouched. One is_allocated test per index.
    [[nodiscard]] Image& IndexImage(ImageId id, [[maybe_unused]] const std::source_location& loc =
                                                  std::source_location::current()) {
#ifdef BB_UWP
        if (!slot_images.is_allocated(id)) {
            ReportDeadImage(id, "index slot_images", loc);
        }
#endif
        return slot_images[id];
    }
    [[nodiscard]] const Image& IndexImage(
        ImageId id, [[maybe_unused]] const std::source_location& loc =
                        std::source_location::current()) const {
#ifdef BB_UWP
        if (!slot_images.is_allocated(id)) {
            ReportDeadImage(id, "index slot_images", loc);
        }
#endif
        return slot_images[id];
    }
    /// bbport: the same for image views; a view id outliving its image is the other way these
    /// assertions are reached.
    [[nodiscard]] ImageView& IndexImageView(
        ImageId id, [[maybe_unused]] const std::source_location& loc =
                        std::source_location::current()) {
#ifdef BB_UWP
        if (!slot_image_views.is_allocated(id)) {
            ReportDeadImage(id, "index slot_image_views", loc);
        }
#endif
        return slot_image_views[id];
    }
    /// bbport: `loc` is the caller's, captured by default: when `id` names a freed slot the report
    /// printed before the (unmodified) slot_vector.h assertion says which call site held the stale
    /// id, instead of leaving a bare "invalid or freed slot index" to reason back from. Costs one
    /// pointer-sized argument: the report itself is a branch that never runs in a working build.
    [[nodiscard]] Image& GetImage(ImageId id, [[maybe_unused]] const std::source_location& loc =
                                                 std::source_location::current()) {
#ifdef BB_UWP
        if (!slot_images.is_allocated(id)) {
            ReportDeadImage(id, "GetImage", loc);
        }
#endif
        auto& image = slot_images[id];
        TouchImage(image);
        return image;
    }

    /// Retrieves the image view with the specified id.
    [[nodiscard]] ImageView& GetImageView(
        ImageId id, [[maybe_unused]] const std::source_location& loc =
                        std::source_location::current()) {
#ifdef BB_UWP
        if (!slot_image_views.is_allocated(id)) {
            ReportDeadImage(id, "GetImageView", loc);
        }
#endif
        return slot_image_views[id];
    }

    /// bbport: ids the draw being recorded has already resolved (ImageId of a colour/depth target
    /// or of a binding), whether or not their binding flags are set yet. The collector must not
    /// free them: pass 2 of the binding walk re-indexes these ids. Same thread as the rasterizer,
    /// which is what owns the flags consulted above; cleared when the draw's bindings are reset.
    void HoldCollectedImage(ImageId id) {
        collected_images.push_back(id);
    }

    void ClearCollectedImages() {
        collected_images.clear();
    }

    [[nodiscard]] bool IsImageCollected(ImageId id) const noexcept {
        for (const ImageId held : collected_images) {
            if (held == id) {
                return true;
            }
        }
        return false;
    }

    /// Get the associated depth stencil image if it is still valid.
    ImageId GetAssociatedDepth(Image& image, vk::Format format) {
        if (!image.depth_id) {
            return {};
        }
        // Only 8-bit integer/unorm color aliases can be stencil exports of a depth target; a
        // wider color view at a reused guest address is an ordinary color target.
        if (format != vk::Format::eS8Uint && format != vk::Format::eR8Uint &&
            format != vk::Format::eR8Unorm) {
            image.DisassociateDepth();
            return {};
        }
        if (slot_images.is_allocated(image.depth_id)) {
            auto& depth_image = slot_images[image.depth_id];
            if (depth_image.image_uid == image.depth_uid &&
                depth_image.flags & ImageFlagBits::Registered) {
                return image.depth_id;
            }
        }
        // The linked depth image is no longer valid, disassociate it.
        image.DisassociateDepth();
        return {};
    }

    enum class MetaType {
        CMask,
        FMask,
        HTile,
    };

    /// Returns meta type if the specified address is a metadata surface.
    std::optional<MetaType> IsMeta(VAddr address) const {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            return it->second.type;
        }
        return std::nullopt;
    }

    /// Returns true if a slice of the specified metadata surface has been cleared.
    bool IsMetaCleared(VAddr address, u32 slice) const {
        const auto& it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            return it.value().clear_mask & (1u << slice);
        }
        return false;
    }

    /// Clears all slices of the specified metadata surface.
    bool ClearMeta(VAddr address) {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            it.value().clear_mask = u32(-1);
            return true;
        }
        return false;
    }

    /// Updates the state of a slice of the specified metadata surface.
    bool TouchMeta(VAddr address, u32 slice, bool is_clear) {
        auto it = surface_metas.find(address);
        if (it != surface_metas.end()) {
            if (is_clear) {
                it.value().clear_mask |= 1u << slice;
            } else {
                it.value().clear_mask &= ~(1u << slice);
            }
            return true;
        }
        return false;
    }

    /// Runs the garbage collector.
    void RunGarbageCollector();

    template <typename Func>
    void ForEachImageInRegion(VAddr cpu_addr, size_t size, Func&& func) {
        using FuncReturn = typename std::invoke_result<Func, ImageId, Image&>::type;
        static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
        ImageIds images;
        ForEachPage(cpu_addr, size, [this, &images, cpu_addr, size, func](u64 page) {
            const auto it = page_table.find(page);
            if (it == nullptr) {
                if constexpr (BOOL_BREAK) {
                    return false;
                } else {
                    return;
                }
            }
            for (const ImageId image_id : *it) {
                Image& image = IndexImage(image_id);
                if (image.flags & ImageFlagBits::Picked) {
                    continue;
                }
                if (!image.Overlaps(cpu_addr, size)) {
                    continue;
                }
                image.flags |= ImageFlagBits::Picked;
                images.push_back(image_id);
                if constexpr (BOOL_BREAK) {
                    if (func(image_id, image)) {
                        return true;
                    }
                } else {
                    func(image_id, image);
                }
            }
            if constexpr (BOOL_BREAK) {
                return false;
            }
        });
        for (const ImageId image_id : images) {
            IndexImage(image_id).flags &= ~ImageFlagBits::Picked;
        }
    }

private:
    /// Iterate over all page indices in a range
    template <typename Func>
    static void ForEachPage(PAddr addr, size_t size, Func&& func) {
        static constexpr u64 MaxAddress = u64{1} << 40;
        if (size == 0 || addr >= MaxAddress) {
            return;
        }
        static constexpr bool RETURNS_BOOL = std::is_same_v<std::invoke_result<Func, u64>, bool>;
        const u64 max_len = MaxAddress - addr;
        const u64 page_end = (addr + std::min<u64>(size, max_len) - 1) >> Traits::PageBits;
        for (u64 page = addr >> Traits::PageBits; page <= page_end; ++page) {
            if constexpr (RETURNS_BOOL) {
                if (func(page)) {
                    break;
                }
            } else {
                func(page);
            }
        }
    }

    /// Copies image memory back to CPU.
    void DownloadImageMemory(ImageId image_id, bool sync = false);

    /// Thread function for copying downloaded images out to CPU memory.
    void DownloadedImagesThread(const std::stop_token& token);

    /// Create an image from the given parameters
    [[nodiscard]] ImageId InsertImage(const ImageInfo& info, VAddr cpu_addr);

    /// Register image in the page table
    void RegisterImage(ImageId image);

    /// Unregister image from the page table
    void UnregisterImage(ImageId image);

    /// Track CPU reads and writes for image
    void TrackImage(ImageId image_id);
    void TrackImageHead(ImageId image_id);
    void TrackImageTail(ImageId image_id);

    /// Stop tracking CPU reads and writes for image
    void UntrackImage(ImageId image_id);
    void UntrackImageHead(ImageId image_id);
    void UntrackImageTail(ImageId image_id);

    void MarkAsMaybeDirty(ImageId image_id, Image& image);
    static u64 MaybeDirtyHash(const Image& image);

    /// Removes the image and any views/surface metas that reference it.
    /// bbport: now: at once, for an image the GPU is done with (ReclaimMemoryFor).
    void DeleteImage(ImageId image_id, bool now = false, const char* reason = "other");
    /// Destroys the image and its views (DeleteImage, once the GPU is done with them).
    void EraseImage(ImageId image_id, const char* reason = "other");

    /// bbport: a new image in slot_images; out of GPU memory for it, frees unused images and
    /// tries again (ReclaimMemoryFor) rather than ending the game.
    ImageId InsertImage(const ImageInfo& info);

    /// bbport: destroys at once up to `bytes` of images unused for a few seconds that can be
    /// recreated from guest memory, none overlapping [addr, addr + size). Returns bytes freed.
    u64 ReclaimMemoryFor(VAddr addr, size_t size, u64 bytes);

    /// Touch the image in the LRU cache.
    void TouchImage(const Image& image);

    /// bbport: `reason` names the caller in the BB_UWP free trace ("eviction", "unmap",
    /// "make-room", "other"); it must be a literal - the last-freed ring keeps the pointer.
    void FreeImage(ImageId image_id, const char* reason = "other") {
        UntrackImage(image_id);
        UnregisterImage(image_id);
        DeleteImage(image_id, false, reason);
    }

    void GarbageCollectImages();
    void GarbageCollectSamplers();

private:
    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    Vulkan::Runtime& runtime;
    AmdGpu::Liverpool* liverpool;
    BufferCache& buffer_cache;
    PageManager& tracker;
    BlitHelper blit_helper;
    TileManager tile_manager;
    Common::SlotVector<Image> slot_images;
    Common::SlotVector<ImageView> slot_image_views;
    tsl::robin_map<u64, Sampler> samplers;
    std::unordered_set<ImageId> download_images;
    u64 total_used_memory = 0;
    u64 gc_evictions = 0, gc_downloads = 0, gc_dropped = 0; ///< bbport: pressure report
    /// bbport: depth/stencil targets reclaimed under pressure without a write-back.
    u64 gc_depth = 0;
    /// bbport: candidates the pin rules kept (bound / render target / scene proxy / video-out).
    u64 gc_pinned = 0;
    u64 gc_kept = 0, gc_kept_bytes = 0; ///< bbport: old images the latest pass could not evict
    std::chrono::steady_clock::time_point gc_report_time{};
    /// bbport: registered images and their guest size, for the once-a-minute memory line.
    u64 image_count = 0, image_bytes = 0;
    std::chrono::steady_clock::time_point memory_report_time{};
    void ReportMemory();
    /// bbport: a freed slot id was indexed: print the id, the call site and the last few frees,
    /// then let the unmodified slot_vector.h assertion stop the GPU. BB_UWP only (called from the
    /// accessors above).
    void ReportDeadImage(ImageId id, const char* accessor, const std::source_location& loc) const;
    /// bbport: the last 20 frees (ring), for ReportDeadImage. Static-initialised, no allocation.
    struct FreedImage {
        u32 id = 0;
        u64 uid = 0;
        const char* reason = "other";
    };
    static constexpr u32 FreedHistorySize = 20;
    std::array<FreedImage, FreedHistorySize> freed_history{};
    u32 freed_history_head = 0;
    u32 freed_history_count = 0;
    void RememberFreed(ImageId id, u64 uid, const char* reason);
    /// bbport: ids resolved by the draw being recorded (HoldCollectedImage).
    std::vector<ImageId> collected_images;
    /// bbport: ReclaimMemoryFor with an explicit age gate - images unused for `unused_seconds`
    /// only. MakeRoomForImage (InsertImage) lowers it round by round: 16 s, then 4 s, then 2 s.
    u64 ReclaimMemoryForUnused(VAddr addr, size_t size, u64 bytes, u32 unused_seconds);
    /// bbport: gc_tick at each of the last 64 seconds (GarbageCollectImages).
    std::array<u64, 64> gc_tick_at_second{};
    u64 gc_second = 0;
    u64 trigger_gc_memory = 0;
    u64 pressure_gc_memory = 0;
    u64 critical_gc_memory = 0;
    u64 total_used_samplers = 0;
    u64 trigger_gc_samplers = 0;
    u64 pressure_gc_samplers = 0;
    u64 critical_gc_samplers = 0;
    u64 gc_tick = 0;
    Common::LeastRecentlyUsedCache<ImageId, u64> lru_cache;
    Common::LeastRecentlyUsedCache<u64, u64> sampler_lru_cache;
    const bool readback_linear_images;
    PageTable page_table;
    std::mutex mutex;
    // bbport: FindImage results for unchanged image registrations (guarded by `mutex`).
    struct FindImageCacheEntry {
        VAddr address = 0;
        u64 size = 0;
        Extent3D extent{};
        vk::Format format{};
        AmdGpu::ImageType type{};
        bool exact_fmt = false;
        BindingType binding{};
        u32 levels = 0;
        u32 layers = 0;
        u64 generation = ~0ULL;
        ImageId image_id{};
        int view_mip = -1;
        int view_slice = -1;
    };
    std::array<FindImageCacheEntry, 1024> find_image_cache{};
    std::atomic<u64> registry_generation{0};
    std::mutex samplers_mutex;
    std::mutex download_images_mutex;
    struct MetaDataInfo {
        MetaType type;
        s32 clear_mask = -1;
    };
    tsl::robin_map<VAddr, MetaDataInfo> surface_metas;
};

} // namespace VideoCore
