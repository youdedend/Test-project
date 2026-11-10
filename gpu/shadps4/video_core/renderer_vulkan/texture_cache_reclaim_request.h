// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (diag-2): the hand-off between the watchdog's recovery step and the render thread.
//
// Why this exists: recovery step 1 used to free texture-cache images itself, on the watchdog's
// worker thread, while the render thread was recording copies against those same images. The
// index paths that record copies do not take the cache's mutex, so an image could be erased under
// the copy builder - the stale-image fault in `dzn_image_get_copy_loc`
// (`vulkan_dzn.dll+0x1fd79`) that both 26314 runs died on. Asking here and performing in
// Rasterizer::OnSubmit is the same rule the staging pool already follows (RequestIdleTrim).
//
// Header-only and dependency-free so the policy can be built and tested on the host.
#pragma once

#include <atomic>
#include <cstdint>

namespace BbUwp {

class ReclaimRequest {
public:
    /// Ask for a reclaim. Safe to call from any thread; frees nothing here.
    void Request(std::uint64_t bytes) noexcept {
        const std::uint64_t wanted = bytes != 0 ? bytes : kDefaultBudget;
        std::uint64_t held = pending_bytes.load(std::memory_order_relaxed);
        while (wanted > held &&
               !pending_bytes.compare_exchange_weak(held, wanted, std::memory_order_relaxed)) {
        }
        requested.store(true, std::memory_order_release);
    }

    /// Take the pending budget (0 when nothing was asked). Render thread only.
    [[nodiscard]] std::uint64_t Take() noexcept {
        if (!requested.load(std::memory_order_acquire)) {
            return 0;
        }
        const std::uint64_t bytes = pending_bytes.exchange(0, std::memory_order_relaxed);
        requested.store(false, std::memory_order_release);
        return bytes != 0 ? bytes : kDefaultBudget;
    }

    /// Record what the performer actually freed, for the memory report.
    void NoteFreed(std::uint64_t bytes) noexcept {
        freed_total.fetch_add(bytes, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t FreedTotal() const noexcept {
        return freed_total.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool Pending() const noexcept {
        return requested.load(std::memory_order_acquire);
    }

    static constexpr std::uint64_t kDefaultBudget = std::uint64_t{32} << 20;

private:
    std::atomic<bool> requested{false};
    std::atomic<std::uint64_t> pending_bytes{0};
    std::atomic<std::uint64_t> freed_total{0};
};

} // namespace BbUwp
