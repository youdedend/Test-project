// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#ifdef BB_UWP
#include "../../../../xbox/graphics/staging_arena.h"
#endif

namespace Vulkan {

class Instance;
class Scheduler;

struct StagingBufferRef {
    VideoCore::Buffer* buffer{};
    u64 offset{};
    u64 size{};
    u8* mapped{};
    VideoCore::MemoryType type{};

    void Flush() const noexcept {
        buffer->Flush(offset, size);
    }

    void Invalidate() const noexcept {
        buffer->Invalidate(offset, size);
    }
};

class StagingBufferPool {
public:
    explicit StagingBufferPool(const Instance& instance, Scheduler& scheduler);
    ~StagingBufferPool();

    StagingBufferPool(const StagingBufferPool&) = delete;
    StagingBufferPool& operator=(const StagingBufferPool&) = delete;

    /// Requests size bytes of memory of the given type.
    [[nodiscard]] StagingBufferRef Request(u64 size, VideoCore::MemoryType type, u64 alignment = 0,
                                           bool deferred = false);

    /// Releases a deferred allocation. It becomes reusable once the current tick completes.
    void FreeDeferred(const StagingBufferRef& ref);

    /// Releases idle memory.
    void TickFrame();

    /// bbport (BUILD 3, item 15a): the watchdog's "trim staging" step, and the bytes it returned.
    ///
    /// It applies exactly the rule TickFrame already applies - a ring block beyond the ring's keep
    /// count and idle for RING_IDLE_FRAMES, a large buffer idle for LARGE_IDLE_FRAMES and whose tick
    /// has been signalled - so it can never destroy a block a command recorded a frame or two ago.
    /// (An urgent one-idle-frame trim did exactly that: cosmo/gow3_xbox's note on TickFrame.) The
    /// difference from TickFrame is that it runs now, from the watchdog, instead of waiting for the
    /// next frame, which is what matters when no frame is being presented.
    [[nodiscard]] u64 TrimIdle();

    /// Called from the watchdog thread. Actual trimming is performed by TickFrame on the
    /// renderer thread; the ring vectors and outstanding refs are not safe to mutate off-thread.
    void RequestIdleTrim() noexcept { trim_requested.store(true, std::memory_order_relaxed); }

    /// bbport (diag-1): read-only counters for the UWP memory table (bbgpu_uwp_memory_cats), the
    /// split step-1 needed to measure: o1heap arena hosting vs the legacy dedicated large buffers.
    /// Plain reads of the same counters ResidentBytes/TickFrame already use; no allocation.
    struct Stats {
        u64 arena_bytes{};
        u64 arena_chunks{};
        u64 arena_records{};
        u64 dedicated_bytes{};
        u64 dedicated_count{};
        /// bbport (diag-2): the ring blocks (fixed BLOCK_SIZE blocks kept between frames) and the
        /// full resident total, so the row cannot read zero while the pool is holding memory.
        u64 ring_bytes{};
        u64 ring_blocks{};
        u64 resident_bytes{};
    };
    [[nodiscard]] Stats GetStats() const;

private:
    struct Block {
        std::unique_ptr<VideoCore::StreamBuffer> buffer;
        u64 last_used_frame;
    };

    struct Ring {
        std::vector<Block> blocks;
        size_t current = 0;
    };

    struct LargeBuffer {
        std::unique_ptr<VideoCore::Buffer> buffer;
        u64 tick;
        u64 last_used_frame;
        bool held;
    };

    static constexpr size_t NUM_TYPES = 3;

    StagingBufferRef RequestFromRing(Ring& ring, u64 size, u64 alignment,
                                     VideoCore::MemoryType type);
    StagingBufferRef RequestLarge(u64 size, VideoCore::MemoryType type, bool deferred);

    void TrimRing(Ring& ring, u64 keep, u64 idle_frames);
    void TrimLarge(std::vector<LargeBuffer>& cache);

    void PublishStats() const;

    /// bbport: the bytes this pool is holding, for TrimIdle's "what did that free" line.
    [[nodiscard]] u64 ResidentBytes() const;

    const Instance& instance;
    Scheduler& scheduler;

    std::array<Ring, NUM_TYPES> rings;
    u64 keep_blocks = 0; ///< bbport: ring blocks never trimmed
    u64 keep_vram_blocks = 0; ///< bbport: the same for the VRAM ring (DeviceLocal)
    std::array<std::vector<LargeBuffer>, NUM_TYPES> large_caches;
#ifdef BB_UWP
    /// Item 2 step 1: o1heap sub-allocation arenas for the large/deferred path (host types only -
    /// DeviceLocal chunks cannot host o1heap metadata). Null entry = legacy path for that type.
    std::array<std::unique_ptr<BbUwp::StagingArena>, NUM_TYPES> arenas;
#endif
    u64 frame = 0;
    std::atomic<bool> trim_requested{false};
};

} // namespace Vulkan
