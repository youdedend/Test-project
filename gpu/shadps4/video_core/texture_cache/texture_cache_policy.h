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

} // namespace BbGpu
