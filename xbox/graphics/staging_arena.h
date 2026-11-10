// SPDX-License-Identifier: GPL-2.0-or-later
// Item 2 step 1 (xbox-swap study, steal #2): o1heap-backed sub-allocation arena for the staging
// pool's large/deferred path. One GPU-visible host chunk set replaces per-request dedicated
// buffers, so a freed region can serve a later request of ANY size once its GPU tick is free -
// the fragmentation the legacy best-fit-in-1.25x window cannot recover.
//
// Pure header (no Vulkan, no WinRT): the pool supplies chunks through function objects and the
// GPU-tick rule through a predicate, so xbox/tests/staging_arena_test.cpp pins the semantics on
// the build machine. Invariants kept from the legacy path:
//   * a sub-allocation is reused only after scheduler.IsFree(tick) (same rule as LargeBuffer);
//   * `held` allocations are released only via FreeDeferredByOffset (deferred=true requests);
//   * anything that does not fit returns failure and the caller falls back to the legacy path -
//     the arena may under-deliver, it may never fail an allocation the old code could serve.
#pragma once

#include "o1heap/o1heap.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <vector>

namespace BbUwp {

class StagingArena {
public:
    struct Chunk {
        void* provider_ctx{};   // the pool's Buffer* (opaque here)
        void* mapped{};         // CPU address; o1heap arena base
        std::uint64_t size{};   // chunk bytes
        O1HeapInstance* heap{}; // o1heap over [mapped, mapped + size)
        std::uint32_t live{};   // records not yet returned to o1heap
        std::uint64_t tick{};   // last GPU tick this chunk served
        std::uint64_t frame{};  // last frame this chunk served
    };

    struct Record {
        std::uint32_t chunk{};  // index into chunks
        void* base{};           // o1heap pointer (free this)
        std::uint64_t offset{}; // aligned offset from chunk base (StagingBufferRef.offset)
        std::uint64_t size{};   // requested bytes
        std::uint64_t tick{};   // GPU tick at hand-out
        bool held{};            // deferred: released only via FreeDeferredByOffset
    };

    struct Hit {
        std::uint32_t record{};
        std::uint64_t offset{};
        void* provider_ctx{}; // the chunk's opaque handle (the pool's Buffer*)
        void* mapped{};       // CPU address of offset (for StagingBufferRef.mapped)
    };

    /// Creates/destroys GPU-visible host memory for a chunk. `make` receives bytes and fills
    /// provider_ctx/mapped/size; false = cannot grow (the request then fails to the legacy path).
    using MakeChunkFn = std::function<bool(std::uint64_t bytes, void*& provider_ctx, void*& mapped)>;
    using ReleaseChunkFn = std::function<void(void* provider_ctx)>;

    struct Config {
        std::uint64_t min_chunk = 32ull << 20; // grow at least this much at a time
        std::uint64_t idle_frames = 120;       // LARGE_IDLE_FRAMES rule for empty chunks
    };

    StagingArena(MakeChunkFn make, ReleaseChunkFn release) : StagingArena{std::move(make), std::move(release), Config{}} {}
    StagingArena(MakeChunkFn make, ReleaseChunkFn release, Config cfg)
        : make{std::move(make)}, release{std::move(release)}, cfg{cfg} {}

    ~StagingArena() {
        for (auto& c : chunks) {
            if (c.provider_ctx) {
                release(c.provider_ctx);
            }
        }
    }

    /// Sub-allocates `size` bytes aligned to `align` (0 = o1heap's 16). Tries existing chunks
    /// first-fit; grows by max(min_chunk, request rounded up to 1 MiB) when none fit. Returns
    /// nothing when the arena cannot serve the request - the caller must then use its legacy path.
    [[nodiscard]] std::optional<Hit> Request(std::uint64_t size, std::uint64_t align,
                                             std::uint64_t tick, bool held) {
        if (size == 0) {
            return std::nullopt;
        }
        const std::uint64_t want = align > O1HEAP_ALIGNMENT ? size + align : size;
        for (std::uint32_t i = 0; i < chunks.size(); ++i) {
            if (auto hit = TryChunk(i, size, align, want, tick, held)) {
                return hit;
            }
        }
        // Grow: o1heap rounds every allocation up to a power-of-two span (o1heap.c:388-390,
        // roundUpToPowerOf2(amount + O1HEAP_ALIGNMENT)), so a request can consume twice its
        // bytes. Budget the span and give the chunk 50% headroom for later smaller requests.
        std::uint64_t span = O1HEAP_ALIGNMENT;
        while (span < want + O1HEAP_ALIGNMENT) {
            span <<= 1;
        }
        std::uint64_t bytes = span + (span >> 1);
        if (bytes < cfg.min_chunk) {
            bytes = cfg.min_chunk;
        }
        void* ctx = nullptr;
        void* mapped = nullptr;
        if (!make(bytes, ctx, mapped) || !mapped) {
            return std::nullopt;
        }
        Chunk c;
        c.provider_ctx = ctx;
        c.mapped = mapped;
        c.size = bytes;
        c.heap = o1heapInit(mapped, static_cast<size_t>(bytes));
        if (!c.heap) { // unaligned base or too small: never keep a half-initialized chunk
            release(ctx);
            return std::nullopt;
        }
        chunks.push_back(c);
        return TryChunk(static_cast<std::uint32_t>(chunks.size() - 1), size, align, want, tick, held);
    }

    /// The deferred-release rule: the record stays allocated until its tick is free and only then
    /// returns to the arena. `buffer_ctx` is the pool's Buffer* (chunk provider_ctx).
    bool FreeDeferredByOffset(void* buffer_ctx, std::uint64_t offset, std::uint64_t tick) {
        for (auto& r : records) {
            const Chunk& c = chunks[r.chunk];
            if (c.provider_ctx == buffer_ctx && r.offset == offset) {
                r.held = false;
                r.tick = tick;
                return true;
            }
        }
        return false;
    }

    /// Reclaims records whose tick is free and releases empty chunks past the idle window.
    /// `is_free` is the scheduler's IsFree; call from TickFrame (renderer thread) only.
    void Collect(std::uint64_t now_frame, const std::function<bool(std::uint64_t)>& is_free) {
        for (std::size_t i = records.size(); i-- > 0;) {
            Record& r = records[i];
            if (r.held || !is_free(r.tick)) {
                continue;
            }
            o1heapFree(chunks[r.chunk].heap, r.base);
            chunks[r.chunk].live -= 1;
            records[i] = records.back();
            records.pop_back();
        }
        for (std::uint32_t i = chunks.size(); i-- > 0;) {
            Chunk& c = chunks[i];
            if (c.live != 0) {
                continue;
            }
            if (now_frame - c.frame < cfg.idle_frames || !is_free(c.tick)) {
                continue;
            }
            release(c.provider_ctx);
            chunks[i] = chunks.back();
            chunks.pop_back();
            // Records store chunk indices; swap-pop moved the last chunk into slot i. Since the
            // released chunk had live == 0 no record references it, but the moved chunk's records
            // still point at the old index - fix them.
            for (auto& r : records) {
                if (r.chunk == chunks.size()) {
                    r.chunk = i;
                }
            }
        }
    }

    [[nodiscard]] bool Owns(const void* buffer_ctx) const {
        for (const auto& c : chunks) {
            if (c.provider_ctx == buffer_ctx) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::uint64_t ResidentBytes() const {
        std::uint64_t bytes = 0;
        for (const auto& c : chunks) {
            bytes += c.size;
        }
        return bytes;
    }

    [[nodiscard]] std::uint64_t LiveBytes() const {
        std::uint64_t bytes = 0;
        for (const auto& r : records) {
            bytes += r.size;
        }
        return bytes;
    }

    [[nodiscard]] std::size_t ChunkCount() const { return chunks.size(); }
    [[nodiscard]] std::size_t RecordCount() const { return records.size(); }

private:
    std::optional<Hit> TryChunk(std::uint32_t idx, std::uint64_t size, std::uint64_t align,
                                std::uint64_t want, std::uint64_t tick, bool held) {
        Chunk& c = chunks[idx];
        void* base = o1heapAllocate(c.heap, static_cast<size_t>(want));
        if (!base) {
            return std::nullopt;
        }
        // Align the buffer-relative offset (that is what GPU copy regions and buffer device
        // addresses see); base itself is O1HEAP_ALIGNMENT-aligned within the chunk.
        std::uintptr_t rel =
            reinterpret_cast<std::uintptr_t>(base) - reinterpret_cast<std::uintptr_t>(c.mapped);
        if (align > O1HEAP_ALIGNMENT) {
            const std::uintptr_t mask = static_cast<std::uintptr_t>(align) - 1;
            rel = (rel + mask) & ~mask;
        }
        Record r;
        r.chunk = idx;
        r.base = base;
        r.offset = static_cast<std::uint64_t>(rel);
        r.size = size;
        r.tick = tick;
        r.held = held;
        records.push_back(r);
        c.live += 1;
        c.tick = tick;
        return Hit{static_cast<std::uint32_t>(records.size() - 1), r.offset, c.provider_ctx,
                   reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(c.mapped) + rel)};
    }

    MakeChunkFn make;
    ReleaseChunkFn release;
    Config cfg;
    std::vector<Chunk> chunks;
    std::vector<Record> records;
};

} // namespace BbUwp
