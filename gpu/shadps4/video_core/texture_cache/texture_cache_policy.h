// SPDX-License-Identifier: GPL-2.0-or-later
// Which texture-cache images the garbage collector may free.
//
// A freed image slot is never safe to index again: Common::SlotId is a bare index without a
// generation, so any component that still holds the id either trips slot_vector.h ValidateIndex
// ("invalid or freed slot index", which stops the GPU) or silently reads whichever image took the
// slot. The collector therefore must not free an image something still refers to.
//
// The rules live here, as plain booleans, so they can be tested without Vulkan
// (xbox/tests/texture_cache_policy_test.cpp). TextureCache::ReclaimMemoryFor has always applied
// them; TextureCache::GarbageCollectImages did not, which is how a bound render target or a live
// video-out surface could be evicted under memory pressure while the rasterizer, the presenter or
// the upscaler still held it. Pending GPU->CPU downloads are not a pin here: DeleteImage removes
// the image from download_images before the slot is freed.
#pragma once

namespace BbGpu {

/// Why the cache must keep an image. Mirrors the checks ReclaimMemoryFor applies before it frees
/// anything.
struct ImagePinState {
    /// The image is bound in a descriptor set for the draw being built (ImageBinding::is_bound).
    bool bound = false;
    /// The image is bound as a render target or depth target (ImageBinding::is_target).
    bool target = false;
    /// The image belongs to the scene-resolution proxy set (Image::scene_proxy).
    bool scene_proxy = false;
    /// The image backs a video-out surface the presenter reads (Image::usage.vo_surface).
    bool vo_surface = false;
    /// The current draw already resolved this id (the rasterizer's bound_images: colour/depth
    /// targets and texture bindings collected before the descriptors are written). Such an image
    /// has no binding flag set yet, so the other three checks do not see it.
    bool collected = false;
};

/// True when the image must not be freed this pass, whatever the collector's age/pressure rules.
[[nodiscard]] inline bool ImagePinned(const ImagePinState& state) {
    return state.bound || state.target || state.scene_proxy || state.vo_surface || state.collected;
}

/// The collector's decision for one candidate, given whether the age/pressure rules selected it.
/// A pinned image is never freed, and it does not consume a deletion either: the caller keeps
/// iterating (returns false), so a run of pinned entries cannot stall the pass.
[[nodiscard]] inline bool GcMayFreeImage(const ImagePinState& state, bool age_allows_free) {
    return age_allows_free && !ImagePinned(state);
}

// ---------------------------------------------------------------------------------------------
// BUILD 2: why a candidate the age rules selected still does not go, and when the collector should
// stay out of the way instead of walking the cache again.
//
// `.717` is the run this comes from: the emergency reclaim found nothing to free, and the log could
// not say whether that was because everything was pinned, because every candidate was a tiled
// GPU-written image that cannot be written back, or because the pass simply ran out of deletions on
// entries it then refused. Those are three different fixes, so they are three different numbers.
// ---------------------------------------------------------------------------------------------

/// Why one selected candidate was not freed. `none` means it was freed (or is being freed now).
enum class GcBlock : unsigned {
    none = 0,
    pinned,             ///< something still holds the id (bound / target / proxy / vo / collected)
    gpu_written_tiled,  ///< GPU-written and tiled: its contents exist only in VRAM and cannot be
                        ///< written back, so freeing it loses them. Retained unless BB_GC_DROP_OLD.
    writeback_budget,   ///< it could go, but this pass has already spent its write-backs
    not_pressured,      ///< GPU-written but the pass may not spend a write-back (below the mark)
    unsupported_layout, ///< the write-back path cannot express this image (kept, never half-written)
    recent,             ///< the age gate above did not select it (counted by the caller)
};

/// How much one collector pass may spend. The write-back cap is the anti-thrash rule that matters
/// most on the console: each one finishes the GPU and copies over the bus, so four of them in a pass
/// already cost a frame or two, and ten of them cost a visible stall without freeing much more.
struct GcPassLimits {
    /// A pass that frees less than this is treated as "the cache has nothing to give" and the next
    /// one is deferred until usage actually grows (see GcShouldBackOff).
    unsigned long long min_useful_bytes = 32ull << 20;
    /// Synchronous GPU->CPU write-backs (each one a full finish + copy) allowed in one pass.
    unsigned writebacks = 4;
    /// Usage must fall to `trigger - hysteresis` before the collector stops, and must reach
    /// `trigger` again before it starts: without the gap, a run sitting on the mark evicts and
    /// re-uploads the same textures every pass.
    unsigned long long hysteresis_bytes = 128ull << 20;
};

/// The state the classification needs beyond the pin flags.
struct GcCandidateState {
    /// Image::SafeToDownload(): GPU-written and not dirty.
    bool gpu_written = false;
    /// ImageInfo::IsTiled(): the guest's layout, which no host write-back can reproduce directly.
    bool tiled = false;
    /// Write-backs already spent by this pass.
    unsigned writebacks_done = 0;
    /// The image's format/layout combination is expressible by the write-back path.
    bool layout_supported = true;
    /// This pass may spend a synchronous write-back (only over the pressure mark).
    bool writeback_allowed = true;
};

/// `allow_drop_old` is BB_GC_DROP_OLD: when on, an old tiled GPU-written image may be dropped
/// unwritten. It is off everywhere by default and off on the console by policy.
[[nodiscard]] inline GcBlock GcClassifyCandidate(const ImagePinState& pins,
                                                 const GcCandidateState& candidate,
                                                 const GcPassLimits& limits, bool allow_drop_old) {
    if (ImagePinned(pins)) {
        return GcBlock::pinned;
    }
    if (candidate.gpu_written && candidate.tiled && !allow_drop_old) {
        return GcBlock::gpu_written_tiled;
    }
    if (candidate.gpu_written && !candidate.layout_supported) {
        return GcBlock::unsupported_layout;
    }
    if (candidate.gpu_written && !candidate.writeback_allowed) {
        return GcBlock::not_pressured;
    }
    if (candidate.gpu_written && candidate.writebacks_done >= limits.writebacks) {
        return GcBlock::writeback_budget;
    }
    return GcBlock::none;
}

/// Should this pass be skipped? True when the previous pass freed almost nothing and nothing has
/// happened since: walking the whole cache again cannot find what it did not find a moment ago, and
/// on this console that walk happens under the cache mutex while the game is submitting commands.
[[nodiscard]] inline bool GcShouldBackOff(unsigned long long last_pass_freed,
                                         unsigned long long usage_growth,
                                         const GcPassLimits& limits) {
    return last_pass_freed < limits.min_useful_bytes && usage_growth < limits.min_useful_bytes * 2;
}

/// Collector on/off across passes: start at `trigger`, stop at `target` (= trigger - hysteresis).
/// `active` is the previous answer, so this is the whole state machine.
[[nodiscard]] inline bool GcHysteresisActive(unsigned long long usage, unsigned long long trigger,
                                             unsigned long long hysteresis, bool active) {
    if (usage >= trigger) {
        return true;
    }
    if (active) {
        const unsigned long long target = trigger > hysteresis ? trigger - hysteresis : 0;
        return usage > target;
    }
    return false;
}

} // namespace BbGpu
