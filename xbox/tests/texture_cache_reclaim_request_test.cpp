// SPDX-License-Identifier: GPL-2.0-or-later
// CI test for the deferred image reclaim (diag-2).
//
// The crash this pins down: `26302.0802` onward, the watchdog's recovery step 1 called
// TextureCache::EmergencyReclaim straight from its own worker thread. The walk takes the cache's
// `mutex`, but the renderer's index paths (GetImage, the binding walks, copy recording) read the
// image slots unlocked, so an image the render-thread copy builder still referred to could be
// erased underneath it. Both 26314 runs died on the resulting stale image in
// `dzn_image_get_copy_loc` (`vulkan_dzn.dll+0x1fd79`, device=0xE0/0xD8, addr=0x150/0x148).
// No build up to `26301.1156` had a watchdog image reclaim, which is why the user's earlier
// versions did not crash.
//
// The rule the fix enforces, mirroring the staging pool's RequestIdleTrim:
//   * a watchdog step may only ASK for a reclaim (nothing is freed on that thread);
//   * exactly one reclaim is performed per request, by the render thread;
//   * a request made while one is pending is coalesced, keeping the larger budget;
//   * a request of 0 means "a budget", never "nothing" (EmergencyReclaim(0) would free nothing).
#include "renderer_vulkan/texture_cache_reclaim_request.h"

#include <cstdio>
#include <string>
#include <thread>

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

constexpr unsigned long long MiB = 1024ull * 1024ull;

} // namespace

int main() {
    // 1. A step with a specific need asks for exactly that much, and frees nothing itself.
    {
        BbUwp::ReclaimRequest request;
        request.Request(72 * MiB);
        Check(request.Pending(), "asking for a reclaim leaves a request pending");
        Check(request.FreedTotal() == 0, "asking for a reclaim frees nothing on the caller's thread");
        const auto taken = request.Take();
        Check(taken == 72 * MiB, "the render thread takes the budget the step wanted");
        Check(!request.Pending(), "the request is cleared once taken");
        Check(request.Take() == 0, "a second take finds nothing to do");
    }

    // 2. Requests coalesce while one is pending, keeping the larger budget: two watchdog steps in
    //    one second must not shrink a 200 MiB ask to 32 MiB, and must not queue two walks.
    {
        BbUwp::ReclaimRequest request;
        request.Request(64 * MiB);
        request.Request(200 * MiB);
        request.Request(96 * MiB);
        Check(request.Take() == 200 * MiB, "coalesced request keeps the largest budget");
        Check(request.Take() == 0, "coalescing leaves exactly one reclaim pending");
    }

    // 3. A zero (or absent) budget means the default 32 MiB walk, never zero.
    {
        BbUwp::ReclaimRequest request;
        request.Request(0);
        Check(request.Take() == (32ull * MiB), "a zero budget becomes the 32 MiB default");
    }

    // 4. The render thread performs it: the freed total accumulates where the report can read it.
    {
        BbUwp::ReclaimRequest request;
        request.Request(48 * MiB);
        const auto budget = request.Take();
        request.NoteFreed(30 * MiB);
        request.Request(48 * MiB);
        request.Take();
        request.NoteFreed(18 * MiB);
        Check(budget == 48 * MiB, "the budget reached the performer");
        Check(request.FreedTotal() == 48 * MiB, "the freed total accumulates across reclaims");
    }

    // 5. A cross-thread request is visible to the performing thread (the real shape: watchdog
    //    worker asks, render thread performs).
    {
        BbUwp::ReclaimRequest request;
        unsigned long long seen = 0;
        request.Request(128 * MiB);
        std::thread performer([&] { seen = request.Take(); });
        performer.join();
        Check(seen == 128 * MiB, "a request from another thread is seen by the performer");
    }

    if (failures != 0) {
        std::printf("texture_cache_reclaim_request: %d FAILED\n", failures);
        return 1;
    }
    std::printf("texture_cache_reclaim_request: PASS\n");
    return 0;
}
