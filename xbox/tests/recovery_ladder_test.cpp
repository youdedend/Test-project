// SPDX-License-Identifier: GPL-2.0-or-later
// Pins the pre-allocation recovery ladder (item 2, stage 2a): rung order, the mid-draw Finish
// skip, early exit once the need is covered, and the no-op cases. Pure header, host-buildable.
#include "../graphics/recovery_ladder.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace {
std::vector<std::string> g_order;
std::uint64_t g_budget = 0; // bytes each rung "frees" (from remaining need)
std::uint64_t g_last_need = 0;

void reset(std::uint64_t frees_per_rung = 0) {
    g_order.clear();
    g_budget = frees_per_rung;
    g_last_need = 0;
}
std::uint64_t rung(const char* name, std::uint64_t need) {
    g_order.push_back(name);
    g_last_need = need;
    return g_budget;
}
} // namespace

int main() {
    using namespace BbUwp;
    constexpr std::uint64_t M = 1ull << 20;

    // 1. Full order, nothing freed: every rung runs in the item-2 order, Finish included.
    {
        reset(0);
        RecoveryHooks h;
        h.finish_gpu = [](std::uint64_t n) { return rung("finish", n); };
        h.pop_pending = [](std::uint64_t n) { return rung("pop", n); };
        h.release_retired = [](std::uint64_t n) { return rung("retired", n); };
        h.reclaim_images = [](std::uint64_t n) { return rung("images", n); };
        h.pager = [](std::uint64_t n) { return rung("pager", n); };
        const auto out = RunRecoveryLadder(h, 64 * M, false);
        assert((g_order == std::vector<std::string>{"finish", "pop", "retired", "images", "pager"}));
        assert(out.rungs_run == 5 && !out.met_need && !out.finish_skipped && out.freed == 0);
    }
    // 2. Mid-draw: Finish is skipped and flagged, the rest runs in order.
    {
        reset(0);
        RecoveryHooks h;
        h.finish_gpu = [](std::uint64_t n) { return rung("finish", n); };
        h.pop_pending = [](std::uint64_t n) { return rung("pop", n); };
        h.release_retired = [](std::uint64_t n) { return rung("retired", n); };
        const auto out = RunRecoveryLadder(h, 8 * M, true);
        assert((g_order == std::vector<std::string>{"pop", "retired"}));
        assert(out.finish_skipped && out.rungs_run == 2 && !out.met_need);
    }
    // 3. Early exit: the first rung covers the need and the expensive walks never run.
    {
        reset(128 * M);
        RecoveryHooks h;
        h.finish_gpu = [](std::uint64_t n) { return rung("finish", n); };
        h.pop_pending = [](std::uint64_t n) { return rung("pop", n); };
        h.release_retired = [](std::uint64_t n) { return rung("retired", n); };
        h.reclaim_images = [](std::uint64_t n) { return rung("images", n); };
        h.pager = [](std::uint64_t n) { return rung("pager", n); };
        const auto out = RunRecoveryLadder(h, 100 * M, true);
        assert((g_order == std::vector<std::string>{"pop"}));
        assert(out.met_need && out.freed == 128 * M && out.rungs_run == 1);
    }
    // 4. Each rung sees the STILL-SHORT amount, not the original need.
    {
        reset(40 * M);
        RecoveryHooks h;
        h.pop_pending = [](std::uint64_t n) { return rung("pop", n); };
        h.reclaim_images = [](std::uint64_t n) { return rung("images", n); };
        const auto out = RunRecoveryLadder(h, 100 * M, true);
        assert(out.rungs_run == 2 && out.freed == 80 * M && !out.met_need);
        assert(g_last_need == 60 * M); // second rung saw 100 - 40
    }
    // 5. need == 0 runs nothing; null hooks are skipped safely; partial coverage reports honestly.
    {
        reset(10 * M);
        RecoveryHooks empty;
        const auto no_op = RunRecoveryLadder(empty, 0, false);
        assert(no_op.rungs_run == 0 && no_op.freed == 0);
        RecoveryHooks only_pager;
        only_pager.pager = [](std::uint64_t n) { return rung("pager", n); };
        const auto partial = RunRecoveryLadder(only_pager, 100 * M, false);
        assert(partial.rungs_run == 1 && partial.freed == 10 * M && !partial.met_need);
    }
    std::puts("recovery ladder: all checks passed");
    return 0;
}
