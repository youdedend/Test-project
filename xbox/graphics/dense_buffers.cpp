// SPDX-License-Identifier: GPL-2.0-or-later
// Xbox UWP dense ordinary-buffer arena backing + logical DMA page-table views
// (Cosmo/uncharted_xbox 5c74fc18 RetireTicks=64 safe deferred arena release).

#include "dense_arena_lifetime.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>

namespace VideoCore {

void BufferCache::InitializeDenseBuffers() {
    block_shift = 16;
    block_size = 1u << block_shift;
    blocks_per_arena_page = arena_page_size / block_size;
    blocks_per_arena_page_shift = arena_page_bits - block_shift;
    const u64 pages = u64{1} << (ADDRESS_SPACE_BITS - block_shift);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift, pages);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, pages * sizeof(u64), MemoryType::DeviceLocal, "Logical DMA Page Table");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0, pages * sizeof(u64), 0);
    null_buffer = std::make_unique<Buffer>(
        instance, 0, 64_KB, MemoryType::DeviceLocal, "Read-only zero binding");
    runtime.FillBuffer(null_buffer.get(), 0, 64_KB, 0);
    runtime.FlushBarriers();
    scheduler.Finish();
    std::printf("UWP buffer cache: ordinary buffers, %llu MiB pages, %u logical DMA views\n",
                static_cast<unsigned long long>(arena_page_size >> 20), DmaViewCount);
}

std::array<vk::DescriptorBufferInfo, BufferCache::DmaViewCount> BufferCache::GetDmaViews() const {
    std::array<vk::DescriptorBufferInfo, DmaViewCount> result;
    for (u32 i = 0; i < DmaViewCount; ++i) {
        const auto& view = dense_views[i];
        result[i] = view.arena ? vk::DescriptorBufferInfo{view.arena->Handle(), view.offset, arena_page_size}
                               : vk::DescriptorBufferInfo{null_buffer->Handle(), 0, null_buffer->SizeBytes()};
    }
    return result;
}

const Buffer* BufferCache::GetDenseArena(u64 first_block, u64 last_block) {
    u64 first_page = first_block >> blocks_per_arena_page_shift;
    u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT(last_page < address_space.size());
    auto existing = address_space[first_page];
    if (existing && existing->cpu_addr <= (first_block << block_shift) &&
        existing->cpu_addr + existing->size_bytes > (last_block << block_shift)) {
        return existing;
    }

    std::set<const Buffer*> previous;
    bool expanded = false;
    do {
        expanded = false;
        for (u64 page = first_page; page <= last_page; ++page) {
            if (auto old = address_space[page]; old && previous.insert(old).second) {
                const u64 old_first = old->cpu_addr >> arena_page_bits;
                const u64 old_last = (old->cpu_addr + old->size_bytes - 1) >> arena_page_bits;
                expanded |= old_first < first_page || old_last > last_page;
                first_page = std::min(first_page, old_first);
                last_page = std::max(last_page, old_last);
            }
        }
    } while (expanded);

    Buffer* arena = nullptr;
    {
        std::scoped_lock lk{dense_arena_mutex};
        arena = &dense_arenas.emplace_back(instance, first_page << arena_page_bits,
                                           (last_page - first_page + 1) << arena_page_bits,
                                           MemoryType::DeviceLocal);
    }

    u64 retired_bytes = 0;
    for (auto old : previous) {
        const vk::BufferCopy copy{0, old->cpu_addr - arena->cpu_addr, old->size_bytes};
        runtime.CopyBuffer(old, arena, std::span{&copy, 1});
        retired_bytes += old->size_bytes;
        arena_backed -= std::min<u64>(arena_backed, old->size_bytes);
    }

    // Replaced arenas hold whole copies of their ranges; keep them alive until every command
    // recorded before the replacement has been signaled (fence) and no draw preparation worker,
    // scanner or recording thread still holds a pointer into them. A tick-based guess alone let
    // Test #11 destroy an arena under a submission that was still being prepared.
    if (!previous.empty()) {
        std::scoped_lock lk{dense_arena_mutex};
        const u64 tick = scheduler.CurrentTick();
        BbUwp::DenseArenas().Retire(tick);
        for (auto it = dense_arenas.begin(); it != dense_arenas.end(); ++it) {
            if (previous.contains(&*it)) {
                retired_arenas.push_back({it, tick});
            }
        }
    }

    arena_backed += arena->size_bytes;
    for (u64 page = first_page; page <= last_page; ++page) {
        address_space[page] = arena;
        auto [entry, inserted] = dense_slots.emplace(page, u32(dense_slots.size()));
        (void)inserted;
        ASSERT_MSG(entry->second < DmaViewCount, "Logical DMA view capacity reached (%u)", DmaViewCount);
        dense_views[entry->second] = {arena, (page - first_page) << arena_page_bits};
    }

    std::printf("UWP dense GPU arena: %#llx, %llu MiB (retired %llu MiB, live %llu MiB), %zu/%u DMA views\n",
                static_cast<unsigned long long>(arena->cpu_addr),
                static_cast<unsigned long long>(arena->size_bytes >> 20),
                static_cast<unsigned long long>(retired_bytes >> 20),
                static_cast<unsigned long long>(arena_backed >> 20),
                dense_slots.size(), DmaViewCount);
    std::fflush(stdout);
    return arena;
}

void BufferCache::ReleaseRetiredArenas() {
    auto& lifetime = BbUwp::DenseArenas();
    const u64 now = scheduler.CurrentTick();
    const u32 inflight = lifetime.Inflight();
    std::size_t deferred = 0;
    std::size_t remaining = 0;
    {
        std::scoped_lock lk{dense_arena_mutex};
        std::erase_if(retired_arenas, [&](const RetiredArena& retired) {
            if (!lifetime.CanRelease(retired.tick, now,
                                     [this](u64 tick) { return scheduler.IsFree(tick); })) {
                deferred += inflight != 0 ? 1 : 0;
                return false;
            }
            std::printf("UWP dense GPU arena: %#llx, %llu MiB released\n",
                        static_cast<unsigned long long>(retired.arena->cpu_addr),
                        static_cast<unsigned long long>(retired.arena->size_bytes >> 20));
            std::fflush(stdout);
            dense_arenas.erase(retired.arena);
            return true;
        });
        remaining = retired_arenas.size();
    }
    // Logged at most once a second: with draw preparation running there is nearly always a
    // producer holding an arena, so a per-call line would flood the log.
    if (deferred != 0) {
        static u64 last_report = 0;
        const u64 ms = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        if (ms - last_report >= 1000) {
            last_report = ms;
            std::printf("UWP dense arena: retiring deferred (inflight=%u, %zu deferred, %zu "
                        "retired)\n",
                        inflight, deferred, remaining);
            std::fflush(stdout);
        }
    }
}

void BufferCache::EnsureDenseResident(const Buffer* arena, u64 first_block, u64 last_block) {
    IntervalList gaps;
    resident_ranges.ForEachGap(first_block, last_block + 1,
                               [&](u64 first, u64 end) { gaps.Add({first, end}); });
    for (auto const& range : gaps) {
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = VK_NULL_HANDLE;
        backing.offset = (range.start << block_shift) - arena->cpu_addr;
        resident_ranges.Add(backing);
        const u64 bytes = (range.end - range.start) * sizeof(u64);
        auto staging = staging_pool.Request(bytes, MemoryType::HostUncached);
        auto entries = reinterpret_cast<u64*>(staging.mapped);
        for (u64 block = range.start; block < range.end; ++block) {
            const u64 address = block << block_shift;
            const u32 slot = dense_slots.at(address >> arena_page_bits);
            *entries++ = (u64(slot + 1) << 32) | (address & (arena_page_size - 1));
        }
        staging.Flush();
        const vk::BufferCopy copy{staging.offset, range.start * sizeof(u64), bytes};
        runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), std::span{&copy, 1});
    }
}

} // namespace VideoCore
