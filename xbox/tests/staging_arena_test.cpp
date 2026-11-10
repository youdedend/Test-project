// SPDX-License-Identifier: GPL-2.0-or-later
// Item 2 step 1 (xbox-swap study, steal #2): host tests for xbox/graphics/staging_arena.h -
// the o1heap sub-allocation arena behind the staging pool's large/deferred path. These pin the
// reuse rules that decide correctness (use-after-free class bugs) and the fragmentation win
// (cross-size reuse the legacy best-fit-in-1.25x window cannot do).
#include "staging_arena.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

namespace {

struct F {
    struct FakeChunk {
        void* ctx;
        void* mapped;
        std::uint64_t size;
        int* alive;
    };

    std::vector<FakeChunk> fake;
    int alive_count = 0;
    std::vector<std::uint64_t> free_ticks = {0}; // ticks the "GPU" has finished with

    BbUwp::StagingArena Make(std::uint64_t min_chunk = 1ull << 20) {
        BbUwp::StagingArena::Config cfg;
        cfg.min_chunk = min_chunk;
        cfg.idle_frames = 10;
        return BbUwp::StagingArena(
            [this](std::uint64_t bytes, void*& ctx, void*& mapped) -> bool {
                void* p = ::operator new(static_cast<size_t>(bytes), std::align_val_t{64}, std::nothrow);
                if (!p) {
                    return false;
                }
                auto* handle = new int(++alive_count);
                fake.push_back(FakeChunk{handle, p, bytes, handle});
                ctx = handle;
                mapped = p;
                return true;
            },
            [this](void* ctx) {
                for (auto it = fake.begin(); it != fake.end(); ++it) {
                    if (it->ctx == ctx) {
                        ::operator delete(it->mapped, std::align_val_t{64});
                        delete it->alive;
                        fake.erase(it);
                        return;
                    }
                }
                assert(false && "release of unknown chunk");
            },
            cfg);
    }

    [[nodiscard]] bool IsFree(std::uint64_t tick) const {
        for (std::uint64_t t : free_ticks) {
            if (t == tick) {
                return true;
            }
        }
        return false;
    }
};

int g_failures = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            g_failures += 1;                                                                       \
        }                                                                                          \
    } while (0)

} // namespace

int main() {
    // 1) Basic alloc, no reuse before the tick is free, reuse after Collect.
    {
        F f;
        auto arena = f.Make();
        CHECK(arena.Request(1ull << 20, 0, 7, false).has_value());
        CHECK(arena.LiveBytes() == (1ull << 20));
        CHECK(arena.ChunkCount() == 1);
        f.free_ticks.clear(); // tick 7 NOT free: nothing is reclaimed
        arena.Collect(1, [&f](std::uint64_t t) { return f.IsFree(t); });
        CHECK(arena.RecordCount() == 1);
        f.free_ticks.push_back(7);
        arena.Collect(2, [&f](std::uint64_t t) { return f.IsFree(t); });
        CHECK(arena.RecordCount() == 0);
        CHECK(arena.LiveBytes() == 0);
    }

    // 2) Cross-size reuse: a freed 20 MiB region serves 5 MiB + 15 MiB in the SAME chunk -
    //    the fragmentation recovery the legacy best-fit cache cannot do.
    {
        F f;
        auto arena = f.Make(32ull << 20);
        CHECK(arena.Request(20ull << 20, 0, 1, false).has_value());
        f.free_ticks.push_back(1);
        arena.Collect(2, [&f](std::uint64_t t) { return f.IsFree(t); });
        CHECK(arena.ChunkCount() == 1);
        CHECK(arena.Request(5ull << 20, 0, 3, false).has_value());
        CHECK(arena.Request(15ull << 20, 0, 3, false).has_value());
        CHECK(arena.ChunkCount() == 1); // no growth needed
    }

    // 3) Alignment: requested aligns are honored relative to the chunk base.
    {
        F f;
        auto arena = f.Make();
        const auto a = arena.Request(4096, 256, 1, false);
        const auto b = arena.Request(4096, 1024, 1, false);
        CHECK(a.has_value() && b.has_value());
        CHECK((a->offset % 256) == 0);
        CHECK((b->offset % 1024) == 0);
    }

    // 4) Held (deferred) records survive Collect until FreeDeferredByOffset, then the tick rule
    //    applies to them too (never freed while the GPU may still read).
    {
        F f;
        auto arena = f.Make();
        auto hit = arena.Request(1ull << 20, 0, 5, true);
        CHECK(hit.has_value());
        void* chunk_ctx = hit->provider_ctx;
        f.free_ticks.push_back(5);
        arena.Collect(2, [&f](std::uint64_t t) { return f.IsFree(t); });
        CHECK(arena.RecordCount() == 1); // still held
        CHECK(arena.FreeDeferredByOffset(chunk_ctx, hit->offset, 9)); // release at tick 9
        arena.Collect(3, [&f](std::uint64_t t) { return f.IsFree(t); });
        CHECK(arena.RecordCount() == 1); // tick 9 not free yet
        f.free_ticks.push_back(9);
        arena.Collect(4, [&f](std::uint64_t t) { return f.IsFree(t); });
        CHECK(arena.RecordCount() == 0);
    }

    // 5) Growth: a request larger than any chunk gets its own; a request that fits nowhere grows.
    {
        F f;
        auto arena = f.Make(1ull << 20);
        CHECK(arena.Request(8ull << 20, 0, 1, false).has_value()); // chunk A >= 8 MiB + slack
        const std::size_t chunks_after_first = arena.ChunkCount();
        CHECK(chunks_after_first == 1);
        CHECK(arena.Request(64ull << 20, 0, 1, false).has_value()); // does not fit A -> new chunk
        CHECK(arena.ChunkCount() == 2);
    }

    // 6) Chunk release: empty chunks past the idle window with a free tick return their memory.
    {
        F f;
        auto arena = f.Make();
        CHECK(arena.Request(4ull << 20, 0, 1, false).has_value());
        f.free_ticks.push_back(1);
        arena.Collect(2, [&f](std::uint64_t t) { return f.IsFree(t); }); // records gone
        CHECK(arena.ResidentBytes() > 0);                               // chunk idle, not yet
        arena.Collect(11, [&f](std::uint64_t t) { return f.IsFree(t); }); // 11-2 >= 10 idle
        CHECK(arena.ResidentBytes() == 0);
        CHECK(arena.ChunkCount() == 0);
    }

    // 7) Fallback signal: when the provider cannot make a chunk, Request fails (the caller must
    //    fall back to the legacy path).
    {
        auto arena = BbUwp::StagingArena(
            [](std::uint64_t, void*&, void*&) { return false; },
            [](void*) {});
        CHECK(!arena.Request(1ull << 20, 0, 1, false).has_value());
    }

    // 8) Owns and FreeDeferredByOffset lookup by (chunk ctx, offset).
    {
        F f;
        auto arena = f.Make();
        auto hit = arena.Request(1ull << 20, 0, 1, true);
        CHECK(hit.has_value());
        CHECK(arena.Owns(hit->provider_ctx));
        CHECK(!arena.Owns(reinterpret_cast<void*>(0x1234)));
        CHECK(!arena.FreeDeferredByOffset(reinterpret_cast<void*>(0x1234), hit->offset, 1));
        CHECK(arena.FreeDeferredByOffset(hit->provider_ctx, hit->offset, 1));
    }

    if (g_failures != 0) {
        std::printf("staging_arena: %d FAILED\n", g_failures);
        return 1;
    }
    std::printf("staging_arena: PASS\n");
    return 0;
}
