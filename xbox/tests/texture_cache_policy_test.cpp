// SPDX-License-Identifier: GPL-2.0-or-later
// CI test for the texture cache's eviction policy (video_core/texture_cache/texture_cache_policy.h)
// and the uid rule that makes a stored ImageId safe to re-check.
//
// The crash this pins down: on the console the collector freed an image that the rasterizer, the
// presenter or the upscaler still held. Common::SlotId is a bare index without a generation, so the
// next lookup either trips slot_vector.h ValidateIndex ("invalid or freed slot index") or reads
// whichever image reused the slot. Two rules keep that from happening:
//   1. an image that something refers to (bound / render target / scene proxy / video-out) is never
//      freed by the collector, and it does not consume a deletion either;
//   2. a stored id is only used again when the image's uid still matches, so a reused slot is
//      detected rather than silently misread.
#include "texture_cache_policy.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    } else {
        std::printf("ok: %s\n", what.c_str());
    }
}

// A stand-in for the two facts the policy depends on: the pin flags and the uid of the image that
// currently occupies a slot.
struct FakeSlot {
    bool allocated = false;
    unsigned long long uid = 0;
    bool bound = false, target = false, scene_proxy = false, vo_surface = false;
    bool collected = false; ///< the draw being recorded already resolved this id

    BbGpu::ImagePinState Pin() const {
        BbGpu::ImagePinState pin;
        pin.bound = bound;
        pin.target = target;
        pin.scene_proxy = scene_proxy;
        pin.vo_surface = vo_surface;
        pin.collected = collected;
        return pin;
    }
};

// Mirrors TextureCache::IsImageCollected over the rasterizer's held set.
bool Collected(const std::vector<int>& held, int id) {
    return std::find(held.begin(), held.end(), id) != held.end();
}

// Mirrors TextureCache::TryGetImage(id, uid): uid 0 is a liveness check, otherwise the uid must
// still match the image in that slot.
bool StillOurs(const FakeSlot& slot, unsigned long long uid) {
    if (!slot.allocated) {
        return false;
    }
    return uid == 0 || slot.uid == uid;
}

void TestUnpinnedImageCanGo() {
    FakeSlot slot;
    slot.allocated = true;
    slot.uid = 7;
    Check(BbGpu::GcMayFreeImage(slot.Pin(), true), "an unbound, unused image is evictable");
    Check(!BbGpu::GcMayFreeImage(slot.Pin(), false), "the age/pressure rules still have to select it");
}

void TestEachPinFlagHoldsTheImage() {
    // One flag at a time: each on its own must stop the collector. These are exactly the checks
    // ReclaimMemoryFor applies, which the GC pass was missing.
    const char* names[] = {"bound", "render target", "scene proxy", "video-out surface",
                           "collected by the draw"};
    for (int i = 0; i < 5; ++i) {
        FakeSlot slot;
        slot.allocated = true;
        slot.uid = 11;
        switch (i) {
        case 0: slot.bound = true; break;
        case 1: slot.target = true; break;
        case 2: slot.scene_proxy = true; break;
        case 3: slot.vo_surface = true; break;
        case 4: slot.collected = true; break;
        }
        Check(BbGpu::ImagePinned(slot.Pin()), std::string("a live ") + names[i] + " pins the image");
        Check(!BbGpu::GcMayFreeImage(slot.Pin(), true),
              std::string("the collector keeps a ") + names[i]);
    }
}

void TestCollectedIdsAreHeld() {
    // The rasterizer resolves an id in pass 1 and binds it in pass 2; between the two the image has
    // no binding flag set, so only the held set stops the collector from freeing it.
    std::vector<int> held{4, 9};
    Check(Collected(held, 4) && Collected(held, 9), "both ids the draw resolved are held");
    Check(!Collected(held, 5), "an id the draw did not resolve is not held");
    FakeSlot slot;
    slot.allocated = true;
    slot.collected = true;
    Check(!BbGpu::GcMayFreeImage(slot.Pin(), true),
          "the collector keeps an id the current draw resolved");
    held.clear();
    slot.collected = Collected(held, 4);
    Check(BbGpu::GcMayFreeImage(slot.Pin(), true),
          "after ResetBindings cleared the held set the image is evictable again");
}

void TestPinCombinations() {
    FakeSlot slot;
    slot.allocated = true;
    Check(!BbGpu::ImagePinned(slot.Pin()), "nothing pinned when no flag is set");
    slot.bound = slot.target = true;
    Check(BbGpu::ImagePinned(slot.Pin()), "several flags still mean pinned");
    slot.bound = slot.target = false;
    slot.scene_proxy = true;
    Check(BbGpu::ImagePinned(slot.Pin()), "a scene proxy alone is enough");
}

void TestPinnedImageDoesNotConsumeDeletions() {
    // The collector must keep iterating past a pinned candidate (clean_up returns false), otherwise a
    // queue of pinned entries would stall the pass: that regression left a 6 GB card over its
    // critical mark with 0 evictions until ErrorOutOfDeviceMemory.
    FakeSlot pinned;
    pinned.allocated = true;
    pinned.target = true;
    FakeSlot freeable;
    freeable.allocated = true;

    unsigned deletions = 0;
    for (const FakeSlot* slot : {&pinned, &pinned, &freeable}) {
        if (!BbGpu::GcMayFreeImage(slot->Pin(), true)) {
            continue; // kept, and no deletion is spent
        }
        ++deletions;
    }
    Check(deletions == 1, "only the freeable image counts against the deletion budget");
}

void TestStoredIdDetectsReusedSlot() {
    // The upscaler stores scene_color together with the uid it had at the time. A slot reused by a
    // different image must not be mistaken for the original.
    FakeSlot slot;
    slot.allocated = true;
    slot.uid = 100;
    const unsigned long long stored_uid = slot.uid;
    Check(StillOurs(slot, stored_uid), "the same image is recognised");

    slot.uid = 101; // a new image took the slot
    Check(!StillOurs(slot, stored_uid), "a reused slot is rejected by the uid");

    slot.allocated = false; // the slot was freed outright
    Check(!StillOurs(slot, stored_uid), "a freed slot is rejected");
    Check(!StillOurs(slot, 0), "a freed slot is rejected by the liveness check too");
}

void TestLivenessOnlyCheck() {
    // Ids owned by another component (CameraMotion::Depth, a draw's parameters) have no uid here:
    // uid 0 asks only whether the slot still exists.
    FakeSlot slot;
    slot.allocated = true;
    slot.uid = 42;
    Check(StillOurs(slot, 0), "a live slot passes the liveness check whatever its uid");
    slot.allocated = false;
    Check(!StillOurs(slot, 0), "a freed slot fails the liveness check");
}

} // namespace

int main() {
    TestUnpinnedImageCanGo();
    TestEachPinFlagHoldsTheImage();
    TestCollectedIdsAreHeld();
    TestPinCombinations();
    TestPinnedImageDoesNotConsumeDeletions();
    TestStoredIdDetectsReusedSlot();
    TestLivenessOnlyCheck();
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("texture cache policy: all checks passed\n");
    return EXIT_SUCCESS;
}
