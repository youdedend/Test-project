// SPDX-License-Identifier: GPL-2.0-or-later
// Xbox UWP pre-allocation recovery ladder (item 2, stage 2a): the ordered rungs from the follow-up
// spec - Finish, PopPendingOperations, retired-arena release, texture emergency reclaim, pager
// sweep - as one tested engine. Like dense_arena_lifetime.h this header is pure (no Vulkan, no
// WinRT) so xbox/tests/recovery_ladder_test.cpp can pin the order and the gating on the build
// machine; gpu/shim/bbgpu.cpp supplies the real rung implementations.
//
// Stage 2a rule: the ladder FREES, it never refuses. A call ends with the allocation proceeding
// exactly as before; denial/split/skip is stage 2b and needs caller contracts (see the handoff
// "Safe continuation for item 2"). The one concession to context: the Finish rung drains the
// submission queue and therefore splits an active render pass, so it is skipped when the caller
// may be mid-draw (handoff failed-attempt #5).
#pragma once

#include <cstdint>
#include <initializer_list>

namespace BbUwp {

enum class RecoveryRung : int {
    FinishGpu = 0,      // scheduler.Finish(): drain queued work so later frees can signal. Mid-draw: skipped.
    PopPending = 1,     // scheduler.PopPendingOperations(): flush recorded ops; safe mid-draw.
    ReleaseRetired = 2, // release replaced dense arenas (signaled ticks, no producer holds), staging idle trim.
    ReclaimImages = 3,  // texture emergency reclaim: images the collector may take, pin rules intact.
    Pager = 4,          // forced guest-page sweep (WC_GARLIC only), bounded by the pager's own budgets.
};

struct RecoveryHooks {
    // Each rung receives the bytes still needed and reports what it freed (0 for rungs whose work
    // is asynchronous, e.g. a staging trim request).
    std::uint64_t (*finish_gpu)(std::uint64_t need) = nullptr;
    std::uint64_t (*pop_pending)(std::uint64_t need) = nullptr;
    std::uint64_t (*release_retired)(std::uint64_t need) = nullptr;
    std::uint64_t (*reclaim_images)(std::uint64_t need) = nullptr;
    std::uint64_t (*pager)(std::uint64_t need) = nullptr;
};

struct RecoveryOutcome {
    std::uint64_t freed = 0;      // total bytes handed back across all rungs run
    int rungs_run = 0;            // how many rungs actually executed
    bool finish_skipped = false;  // Finish was requested but skipped because mid_draw
    bool met_need = false;        // freed >= need before or at the end of the ladder
};

/// Runs the rungs in the fixed item-2 order and stops as soon as `need` is covered: the console
/// has shown a 10 s frame stall from reclaim walks (docs/hardware-test-18), so a ladder that got
/// what it came for must not keep walking. Null hooks are skipped; `need == 0` runs nothing.
inline RecoveryOutcome RunRecoveryLadder(const RecoveryHooks& h, std::uint64_t need,
                                         bool mid_draw) {
    RecoveryOutcome out{};
    if (need == 0) {
        return out;
    }
    const auto remaining = [&]() -> std::uint64_t {
        return out.freed >= need ? 0 : need - out.freed;
    };
    if (h.finish_gpu) {
        if (mid_draw) {
            out.finish_skipped = true; // failed-attempt #5: Finish inside a render pass resubmits it
        } else {
            out.freed += h.finish_gpu(remaining());
            ++out.rungs_run;
        }
    }
    for (std::uint64_t (*rung)(std::uint64_t) : {h.pop_pending, h.release_retired, h.reclaim_images,
                                                 h.pager}) {
        if (out.freed >= need) {
            break;
        }
        if (!rung) {
            continue;
        }
        out.freed += rung(remaining());
        ++out.rungs_run;
    }
    out.met_need = out.freed >= need;
    return out;
}

} // namespace BbUwp
