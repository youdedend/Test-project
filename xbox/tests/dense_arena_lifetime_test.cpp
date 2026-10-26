// SPDX-License-Identifier: GPL-2.0-or-later
// CI test for the dense GPU arena lifetime policy (xbox/graphics/dense_arena_lifetime.h).
//
// It models the Hardware Test #11 situation: an arena is replaced while draw preparation still
// holds it; the old arena must survive until the producer releases it and its recorded
// submissions are signaled. No Vulkan, no guest: plain C++ so it builds and runs on the CI
// runner (xbox.yml: "Build and run dense arena lifetime test").

#include "dense_arena_lifetime.h"

#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

/// A fence that is only "free" for ticks at or below `completed`.
struct FakeFence {
    std::uint64_t completed = 0;
    bool operator()(std::uint64_t tick) const {
        return tick <= completed;
    }
};

void TestDeferredWhilePrepInflight() {
    BbUwp::DenseArenaLifetime lifetime;
    FakeFence fence{};

    lifetime.Retain();   // bb:DrawPrep0 preparing the submission that references the arena
    lifetime.Retire(100);  // a larger arena replaced it while preparation is still running
    fence.completed = 1000;  // every recorded submission has been signaled

    std::size_t deferred = 0;
    Check(lifetime.Collect(fence, 1000, &deferred) == 0,
          "an arena retired while draw preparation holds it is not released");
    Check(deferred == 1, "the deferral is reported (inflight=1)");
    Check(lifetime.RetiredCount() == 1, "the retired arena is still tracked");

    lifetime.Release();  // preparation finished; the draw references the new arena
    Check(lifetime.Collect(fence, 1000, &deferred) == 1,
          "the arena is released once preparation finishes");
    Check(lifetime.RetiredCount() == 0, "no retired arenas remain");
    Check(deferred == 0, "nothing is deferred once the producer released it");
}

void TestInflightProducerBlocksRelease() {
    BbUwp::DenseArenaLifetime lifetime;
    FakeFence fence{};
    fence.completed = 10'000;

    lifetime.Retire(100);
    lifetime.Retain();  // bb:DrawScan holds a pointer into the replaced arena

    std::size_t deferred = 0;
    Check(lifetime.Collect(fence, 10'000, &deferred) == 0, "in-flight producer blocks the release");
    Check(deferred == 1, "the block is reported as deferred (inflight=1)");
    Check(lifetime.RetiredCount() == 1, "the arena stays alive while a producer holds it");

    lifetime.Release();
    Check(lifetime.Collect(fence, 10'000, &deferred) == 1, "release after the producer released it");
}

void TestSignaledAtRetirementIsNotImmediate() {
    BbUwp::DenseArenaLifetime lifetime;
    FakeFence fence{};
    fence.completed = 500;

    lifetime.Retire(500);
    Check(lifetime.Collect(fence, 500) == 0,
          "a just-retired arena is never released in the same tick (recording may be in progress)");
    Check(lifetime.Collect(fence, 500 + BbUwp::DenseArenaLifetime::GraceTicks) == 1,
          "the arena is released when the grace window passes");
}

void TestReleaseNeverUnderflows() {
    BbUwp::DenseArenaLifetime lifetime;
    lifetime.Release();
    lifetime.Release();
    Check(lifetime.Inflight() == 0, "unbalanced Release() does not underflow the counter");
    lifetime.Retain();
    lifetime.Retain();
    lifetime.Release();
    Check(lifetime.Inflight() == 1, "nested Retain/Release pairs are counted");
}

void TestMultipleRetirements() {
    BbUwp::DenseArenaLifetime lifetime;
    FakeFence fence{};
    fence.completed = 1000;

    lifetime.Retire(100);
    lifetime.Retire(200);
    lifetime.Retire(900);
    const std::size_t collected = lifetime.Collect(fence, 950);
    Check(collected == 2, "only arenas past the grace window are released");
    Check(lifetime.RetiredCount() == 1, "the newest arena stays retired");
}

} // namespace

int main() {
    TestDeferredWhilePrepInflight();
    TestInflightProducerBlocksRelease();
    TestSignaledAtRetirementIsNotImmediate();
    TestReleaseNeverUnderflows();
    TestMultipleRetirements();
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("dense arena lifetime: all checks passed\n");
    return EXIT_SUCCESS;
}
