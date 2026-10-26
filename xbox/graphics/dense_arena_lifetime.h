// SPDX-License-Identifier: GPL-2.0-or-later
// Xbox UWP dense GPU arena lifetime: when a dense arena is replaced by a larger one its VkBuffer
// holds whole copies of the replaced ranges, so it must stay alive until every command recorded
// before the replacement has been signaled *and* no producer still holds a pointer into it.
//
// This policy lives in its own header (no Vulkan, no WinRT) so the CI test
// xbox/tests/dense_arena_lifetime_test.cpp can exercise it on the build machine: retiring an
// arena while DrawPrep/DrawScan still hold it must defer the release, never destroy it.

#pragma once

#include <cstdint>
#include <vector>

namespace BbUwp {

class DenseArenaLifetime {
public:
    /// Submissions kept in flight before a replaced arena is even considered: the draw pipe,
    /// prepared draws and copy flushes recorded just after a replacement reference the old
    /// VkBuffer, and Dozen retires resources a few submissions late.
    static constexpr std::uint64_t GraceTicks = 64;

    struct Retired {
        std::uint64_t tick{};
        bool operator==(const Retired&) const = default;
    };

    /// Marks an arena as replaced. `tick` is the scheduler tick at the moment of replacement.
    /// Only the tick is recorded: whether a producer still holds the arena is decided at collect
    /// time from the live counter, so an arena replaced while preparation ran is released as soon
    /// as that preparation finishes (never earlier, never never).
    void Retire(std::uint64_t tick) {
        retired.push_back(Retired{tick});
    }

    /// A producer (draw preparation worker, scanner, recording thread) that may hold a dense
    /// arena pointer for the duration of one submission.
    void Retain() {
        ++inflight;
    }

    void Release() {
        if (inflight != 0) {
            --inflight;
        }
    }

    [[nodiscard]] std::uint32_t Inflight() const {
        return inflight;
    }

    [[nodiscard]] std::size_t RetiredCount() const {
        return retired.size();
    }

    /// Frees every retired arena whose recorded submissions have been signaled and while no
    /// producer holds a dense arena. `is_free(tick)` reports whether every submission up to
    /// `tick` has completed; `deferred` receives how many arenas were blocked only by in-flight
    /// producers (logged as `retiring deferred (inflight=N)`).
    /// Whether the arena retired at `tick` may be destroyed now: no producer holds a dense arena,
    /// the grace window has passed and its recorded submissions have been signaled.
    template <typename IsFree>
    [[nodiscard]] bool CanRelease(std::uint64_t tick, std::uint64_t now_tick,
                                  IsFree&& is_free) const {
        if (inflight != 0) {
            return false;
        }
        if (now_tick < tick || now_tick - tick < GraceTicks) {
            return false;
        }
        return is_free(tick);
    }

    template <typename IsFree>
    std::size_t Collect(IsFree&& is_free, std::uint64_t now_tick, std::size_t* deferred = nullptr) {
        std::size_t collected = 0;
        std::size_t blocked = 0;
        for (auto it = retired.begin(); it != retired.end();) {
            if (CanRelease(it->tick, now_tick, is_free)) {
                it = retired.erase(it);
                ++collected;
            } else {
                blocked += inflight != 0 ? 1 : 0;
                ++it;
            }
        }
        if (deferred) {
            *deferred = blocked;
        }
        return collected;
    }

    std::vector<Retired> retired{};
    std::uint32_t inflight{};
};

/// Process-wide instance used by the buffer cache (retire/collect) and by draw preparation
/// (retain/release). Header-inline so every translation unit shares one counter.
inline DenseArenaLifetime& DenseArenas() {
    static DenseArenaLifetime lifetime;
    return lifetime;
}

} // namespace BbUwp
