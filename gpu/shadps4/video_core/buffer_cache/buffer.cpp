// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <numeric>

#include "common/alignment.h"
#include "common/assert.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include <vk_mem_alloc.h>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstdlib>

#ifdef BB_UWP
#include "../../../../xbox/graphics/arena_admission_policy.h"
extern "C" uint64_t bb_uwp_app_memory_usage(void);
extern "C" uint64_t bb_uwp_app_memory_limit(void);
extern "C" size_t runtime_memory_page_out(size_t target_bytes);
extern "C" uint64_t runtime_memory_pinned_bytes(void);
extern "C" uint64_t bbgpu_uwp_emergency_reclaim(uint64_t need);
/* BUILD 3, item 14: wait for the frees already in flight (retired arenas, images on other threads)
 * and ask again, bounded by a deadline. */
extern "C" uint64_t bbgpu_uwp_wait_for_release(uint64_t need, uint32_t timeout_ms);
/* BUILD 3, item 14: the ladder's last rung - make the next attempt need less (step 3 = degrade). */
extern "C" uint64_t bbgpu_uwp_recovery_step(int32_t step, uint64_t need);
extern "C" uint64_t bb_uwp_app_memory_usage(void);
/* B8: the big/slow allocation line (gpu/shim/bbgpu.cpp). */
extern "C" void bbgpu_note_alloc(uint64_t size, const char *kind, const char *caller,
                                 uint64_t usage_before, double elapsed_ms);

namespace {
/// bbport: how much of the app budget a failed allocation needs back, and the last resort when
/// paging guest pages out was not enough: hand back everything the texture cache may still take.
/// This is the approved ladder (handoff §10) - page out, then images, then let the unmodified
/// assertion stop a run that survives both. Returns the bytes that were released; prints what
/// each step cost, because the same steps run on the GPU thread and the console has shown a 10 s
/// frame stall while they did (docs/hardware-test-18).
u64 UwpEmergencyRelease(u64 size, const char* why) {
    const u64 limit = bb_uwp_app_memory_limit();
    if (limit == 0) {
        return 0;
    }
    const u64 usage = bb_uwp_app_memory_usage();
    const u64 wanted = usage + size + (96ULL << 20) > limit ? usage + size + (96ULL << 20) - limit : 0;
    const auto start = std::chrono::steady_clock::now();
    const u64 guest = wanted ? runtime_memory_page_out(static_cast<size_t>(wanted)) : 0;
    const auto after_guest = std::chrono::steady_clock::now();
    const u64 need_now = bb_uwp_app_memory_usage() + size + (96ULL << 20) > limit
                             ? bb_uwp_app_memory_usage() + size + (96ULL << 20) - limit
                             : 0;
    u64 images = bbgpu_uwp_emergency_reclaim(need_now ? need_now : size);
    // bbport (BUILD 3, item 14): the ladder's waiting rung, between "free images" and "assert".
    //
    // Every earlier rung asks once. Both `.736` and `.819` show the cost of that: the release of a
    // retired arena is deferred while a draw-prep producer is in flight (always, at world load), and
    // the image walk runs under the cache mutex while another thread is inserting - so memory that
    // becomes available a frame later is memory the failed allocation never sees. One bounded wait,
    // not a loop: the budget is `BB_ALLOC_RETRY_MS` (default 250 ms), and after it the ladder still
    // ends at the unmodified assertion rather than waiting forever. Nothing is weakened.
    {
        static const u32 retry_ms = [] {
            const char* env = std::getenv("BB_ALLOC_RETRY_MS");
            const long value = env ? std::strtol(env, nullptr, 10) : 250;
            return static_cast<u32>(std::clamp<long>(value, 0, 2000));
        }();
        const u64 still_short = need_now > images ? need_now - images : 0;
        if (still_short != 0 && retry_ms != 0) {
            images += bbgpu_uwp_wait_for_release(still_short, retry_ms);
        }
    }
    const auto done = std::chrono::steady_clock::now();
    const auto ms = [](auto from, auto to) {
        return static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count());
    };
    std::printf("UWP memory emergency (%s): need %llu MiB; paged out %llu MiB in %lld ms, "
                "freed %llu MiB of images in %lld ms (usage now %llu/%llu MiB)\n",
                why, static_cast<unsigned long long>(wanted >> 20),
                static_cast<unsigned long long>(guest >> 20), ms(start, after_guest),
                static_cast<unsigned long long>(images >> 20), ms(after_guest, done),
                static_cast<unsigned long long>(bb_uwp_app_memory_usage() >> 20),
                static_cast<unsigned long long>(limit >> 20));
    std::fflush(stdout);
    return guest + images;
}
} // namespace
#endif

namespace VideoCore {

std::string_view BufferTypeName(MemoryType type) {
    switch (type) {
    case MemoryType::HostUncached:
        return "HostUncached";
    case MemoryType::HostCached:
        return "HostCached";
    case MemoryType::Stream:
        return "Stream";
    case MemoryType::DeviceLocal:
        return "DeviceLocal";
    case MemoryType::Sparse:
        return "Sparse";
    default:
        return "Invalid";
    }
}

[[nodiscard]] VkMemoryPropertyFlags MemoryUsagePreferredVmaFlags(MemoryType type) {
    return type != MemoryType::DeviceLocal ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                           : VkMemoryPropertyFlagBits{};
}

[[nodiscard]] VmaAllocationCreateFlags MemoryUsageVmaFlags(MemoryType type) {
    switch (type) {
    case MemoryType::HostUncached:
    case MemoryType::Stream:
        return VMA_ALLOCATION_CREATE_MAPPED_BIT |
               VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    case MemoryType::HostCached:
        return VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    case MemoryType::DeviceLocal:
    default:
        return {};
    }
}

[[nodiscard]] VmaMemoryUsage MemoryUsageVma(MemoryType type) {
    switch (type) {
    case MemoryType::DeviceLocal:
    case MemoryType::Stream:
        return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    case MemoryType::HostUncached:
    case MemoryType::HostCached:
        return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    default:
        return VMA_MEMORY_USAGE_UNKNOWN;
    }
}

UniqueBuffer::UniqueBuffer(vk::Device device_, VmaAllocator allocator_)
    : device{device_}, allocator{allocator_} {}

UniqueBuffer::~UniqueBuffer() {
    Destroy();
}

void UniqueBuffer::Destroy() {
    if (allocation) {
        vmaDestroyBuffer(allocator, buffer, allocation);
    } else if (buffer) {
        device.destroyBuffer(buffer);
    }
    buffer = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
    bda_addr = 0;
}

void UniqueBuffer::Create(vk::BufferCreateInfo& buffer_ci, MemoryType mem_type,
                          VmaAllocationInfo* out_alloc_info) {
#ifdef BB_UWP
    buffer_ci.usage &= ~vk::BufferUsageFlagBits::eShaderDeviceAddress;
#endif
    const bool with_bda = bool(buffer_ci.usage & vk::BufferUsageFlagBits::eShaderDeviceAddress);
    if (mem_type != MemoryType::Sparse) {
        const VmaAllocationCreateFlags bda_flag =
            with_bda ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0;
        const VmaAllocationCreateInfo alloc_ci = {
            .flags =
                VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | bda_flag | MemoryUsageVmaFlags(mem_type),
            .usage = MemoryUsageVma(mem_type),
            .requiredFlags = 0,
            .preferredFlags = MemoryUsagePreferredVmaFlags(mem_type),
            .pool = VK_NULL_HANDLE,
            .pUserData = nullptr,
        };

        const VkBufferCreateInfo buffer_ci_unsafe = static_cast<VkBufferCreateInfo>(buffer_ci);
#ifdef BB_UWP
        // Log the requested preflight on *every* large DeviceLocal allocation, including dense
        // replacements: usage already includes the old arenas, so adding the full new size captures
        // the transient peak. This is deliberately observation only: callers assume Create cannot
        // fail, so blocking it would currently produce null-buffer corruption rather than a safe skip.
        if (mem_type == MemoryType::DeviceLocal && buffer_ci.size >= BbArenaAdmission::kMinimum) {
            static const bool observe = [] {
                const char* env = std::getenv("BB_ARENA_ADMISSION");
                return !env || env[0] != '0';
            }();
            const u64 usage = bb_uwp_app_memory_usage();
            const u64 limit = bb_uwp_app_memory_limit();
            if (observe && !BbArenaAdmission::BudgetFits(usage, limit, buffer_ci.size)) {
                std::printf("UWP arena admission OBSERVE ONLY: DeviceLocal request %llu MiB with "
                            "usage %llu/%llu MiB would breach the 250 MiB reserve; allocation NOT "
                            "blocked (safe fallback not implemented)\n",
                            (unsigned long long)(buffer_ci.size >> 20),
                            (unsigned long long)(usage >> 20), (unsigned long long)(limit >> 20));
                std::fflush(stdout);
            }
        }
        if (buffer_ci.size >= (256ULL << 10)) {
            const uint64_t limit = bb_uwp_app_memory_limit();
            if (limit > 0) {
                const uint64_t usage = bb_uwp_app_memory_usage();
                const uint64_t room = buffer_ci.size + (96ULL << 20);
                if (usage + room > limit) {
                    // Ask for what is missing in one go (capped at 256 MiB) instead of a fixed
                    // 64 MiB a pass: a 300 MiB area load needed five passes, each a page-table
                    // sweep on the GPU thread, and the console logged a 10 s frame stall while
                    // they ran.
                    const uint64_t want = std::min<u64>(usage + room - limit, 256ULL << 20);
                    const auto start = std::chrono::steady_clock::now();
                    const size_t paged = runtime_memory_page_out(static_cast<size_t>(want));
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - start)
                                        .count();
                    if (paged != 0) {
                        std::printf("UWP buffer headroom: swap paged out %llu MiB for a %llu KiB "
                                    "allocation in %lld ms (usage %llu/%llu MiB)\n",
                                    static_cast<unsigned long long>(paged >> 20),
                                    static_cast<unsigned long long>(buffer_ci.size >> 10),
                                    static_cast<long long>(ms),
                                    static_cast<unsigned long long>(
                                        bb_uwp_app_memory_usage() >> 20),
                                    static_cast<unsigned long long>(limit >> 20));
                        std::fflush(stdout);
                    }
                    // bbport: when the pager cannot cover the request (`paged < want`: `.823` hit
                    // the disk budget with 916 MiB still pageable, `.736` the same), run the next
                    // rung of the ladder now rather than after the allocation has failed: images
                    // nothing refers to go while the allocation still has a chance. Only in that
                    // case - if the pager covered it there is nothing to do and a walk of the
                    // texture cache would just cost a frame. Same rules as the emergency path
                    // (bound / target / scene proxy / video-out / collected stay), so nothing the
                    // draw in flight is using is taken, and it runs on this thread because the
                    // scheduler is ours here. Bounded to 128 MiB and once a second: `.736` spent
                    // 214 ms freeing 117 MiB, which is a visible hitch if it is repeated.
                    const uint64_t after_page = bb_uwp_app_memory_usage();
                    const uint64_t need = after_page + room > limit ? after_page + room - limit : 0;
                    if (need >= (16ULL << 20) && paged < want) {
                        static auto last_image_reclaim = std::chrono::steady_clock::now() -
                                                        std::chrono::hours(1);
                        const auto now = std::chrono::steady_clock::now();
                        if (now - last_image_reclaim >= std::chrono::seconds(1)) {
                            last_image_reclaim = now;
                            const uint64_t ask = std::min<u64>(need + (32ULL << 20), 128ULL << 20);
                            const auto image_start = std::chrono::steady_clock::now();
                            const uint64_t freed = bbgpu_uwp_emergency_reclaim(ask);
                            const auto image_ms = std::chrono::duration_cast<
                                                      std::chrono::milliseconds>(
                                                      std::chrono::steady_clock::now() -
                                                      image_start)
                                                      .count();
                            if (freed != 0) {
                                std::printf("UWP buffer headroom: freed %llu MiB of images (of "
                                            "%llu MiB asked) for a %llu KiB allocation in %lld "
                                            "ms (usage %llu/%llu MiB)\n",
                                            static_cast<unsigned long long>(freed >> 20),
                                            static_cast<unsigned long long>(ask >> 20),
                                            static_cast<unsigned long long>(buffer_ci.size >> 10),
                                            static_cast<long long>(image_ms),
                                            static_cast<unsigned long long>(
                                                bb_uwp_app_memory_usage() >> 20),
                                            static_cast<unsigned long long>(limit >> 20));
                                std::fflush(stdout);
                            }
                        }
                    }
                }
            }
        }
#endif
        VkBuffer unsafe_buffer{};
        // bbport: B8 - the allocation's size, the console's usage either side of it and how long it
        // took; bbgpu_note_alloc decides whether the line is worth printing (32 MiB or 250 ms).
        const auto uwp_alloc_start = std::chrono::steady_clock::now();
        const uint64_t uwp_usage_before = bb_uwp_app_memory_usage();
        VkResult result = vmaCreateBuffer(allocator, &buffer_ci_unsafe, &alloc_ci, &unsafe_buffer,
                                          &allocation, out_alloc_info);
#ifdef BB_UWP
        for (unsigned attempt = 0; result == VK_ERROR_OUT_OF_DEVICE_MEMORY && attempt < 3;
             ++attempt) {
            const u64 released = UwpEmergencyRelease(buffer_ci.size, "allocation failed");
            // bbport (BUILD 3, item 14): the ladder's last rung before giving up on the *freeing*
            // side - make the next attempt need less. `.819` freed nothing on its final attempt and
            // stopped with the textures already emptied; at that point the only thing left to hand
            // back is the optional work. Done once per failure (a static counter, not per attempt),
            // so a retry loop cannot walk the resolution down by three steps in one call.
            static const bool degrade_on_failure = [] {
                const char* env = std::getenv("BB_ALLOC_DEGRADE");
                return env == nullptr || env[0] != '0';
            }();
            if (degrade_on_failure) {
                static std::atomic<unsigned> degrade_reports{0};
                if (degrade_reports.fetch_add(1) < 4) {
                    std::printf("UWP memory: allocation of %llu KiB failed with the ladder spent; "
                                "degrading optional features and retrying\n",
                                static_cast<unsigned long long>(buffer_ci.size >> 10));
                    std::fflush(stdout);
                    bbgpu_uwp_recovery_step(3, 0);
                }
            }
            if (!released) {
                // Nothing was handed back: the retry below still runs (the degrade above may have
                // been what makes it fit), but a second identical attempt would only repeat it.
                if (attempt != 0) {
                    break;
                }
            }
            result = vmaCreateBuffer(allocator, &buffer_ci_unsafe, &alloc_ci, &unsafe_buffer,
                                     &allocation, out_alloc_info);
        }
#endif
        if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
            // bbport: VRAM full: past the budget, in system memory if need be (see image.cpp).
            Vulkan::NoteOverBudget("buffer");
            VmaAllocationCreateInfo over_budget = alloc_ci;
            over_budget.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
            result = vmaCreateBuffer(allocator, &buffer_ci_unsafe, &over_budget, &unsafe_buffer,
                                     &allocation, out_alloc_info);
            if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
                over_budget.usage = VMA_MEMORY_USAGE_AUTO;
                result = vmaCreateBuffer(allocator, &buffer_ci_unsafe, &over_budget,
                                         &unsafe_buffer, &allocation, out_alloc_info);
            }
        }
#ifdef BB_UWP
        // bbport: B8 - the big and the slow allocations, even when they succeed. This is the call
        // that takes the world from 4.6 GiB to the ceiling, and it used to leave no trace at all.
        bbgpu_note_alloc(buffer_ci.size, BufferTypeName(mem_type).data(), "buffer cache",
                         uwp_usage_before,
                         std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - uwp_alloc_start)
                             .count());
        if (result != VK_SUCCESS) {
            VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
            vmaGetHeapBudgets(allocator, budgets);
            // The pinned part of guest memory is in none of the numbers above but in the total:
            // it is what the pager could not hand back, so it says whether the wall was memory
            // the game pinned or the GPU's own allocations.
            std::printf("[uwp-vk] FATAL vmaCreateBuffer(%llu KiB, type=%.*s) -> %d; "
                        "Xbox AppMemory %llu/%llu MiB (guest pinned=%llu MiB); "
                        "VMA heap[0] block=%llu MiB alloc=%llu MiB usage=%llu MiB budget=%llu MiB\n",
                        static_cast<unsigned long long>(buffer_ci.size >> 10),
                        static_cast<int>(BufferTypeName(mem_type).size()),
                        BufferTypeName(mem_type).data(), static_cast<int>(result),
                        static_cast<unsigned long long>(bb_uwp_app_memory_usage() >> 20),
                        static_cast<unsigned long long>(bb_uwp_app_memory_limit() >> 20),
                        static_cast<unsigned long long>(runtime_memory_pinned_bytes() >> 20),
                        static_cast<unsigned long long>(budgets[0].statistics.blockBytes >> 20),
                        static_cast<unsigned long long>(budgets[0].statistics.allocationBytes >> 20),
                        static_cast<unsigned long long>(budgets[0].usage >> 20),
                        static_cast<unsigned long long>(budgets[0].budget >> 20));
            std::fflush(stdout);
        }
#endif
        ASSERT_MSG(result == VK_SUCCESS, "Failed allocating buffer with error {}",
                   vk::to_string(vk::Result{result}));
        buffer = vk::Buffer{unsafe_buffer};
    } else {
        buffer_ci.flags |=
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency;
        buffer = Vulkan::Check(device.createBuffer(buffer_ci));
    }

    if (with_bda) {
        const vk::BufferDeviceAddressInfo bda_info = {
            .buffer = buffer,
        };
        auto bda_result = device.getBufferAddress(bda_info);
        ASSERT_MSG(bda_result != 0, "Failed to get buffer device address");
        bda_addr = bda_result;
    }
}

Buffer::Buffer(const Vulkan::Instance& instance, VAddr cpu_addr_, u64 size_bytes_,
               MemoryType mem_type_, std::string_view debug_name)
    : cpu_addr{cpu_addr_}, size_bytes{size_bytes_}, mem_type{mem_type_},
      buffer{instance.GetDevice(), instance.GetAllocator()} {

    vk::BufferCreateInfo buffer_ci = {
        .size = size_bytes,
        .usage = AllFlags,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    VmaAllocationInfo alloc_info{};
    buffer.Create(buffer_ci, mem_type, &alloc_info);

    const auto device = instance.GetDevice();
    if (!debug_name.empty()) {
        Vulkan::SetObjectName(device, Handle(), debug_name);
    } else {
        Vulkan::SetObjectName(device, Handle(), "Buffer {:#x}:{:#x}", cpu_addr, size_bytes);
    }
    // bbport: the name in the allocator's statistics (BB_VMA_DUMP), for VRAM by purpose.
    if (buffer.allocation) {
        const std::string name{debug_name.empty() ? std::string_view{"Buffer"} : debug_name};
        vmaSetAllocationName(instance.GetAllocator(), buffer.allocation, name.c_str());
    }

    if (mem_type != MemoryType::Sparse) {
        VkMemoryPropertyFlags property_flags{};
        vmaGetAllocationMemoryProperties(instance.GetAllocator(), buffer.allocation,
                                         &property_flags);
        if (alloc_info.pMappedData) {
            mapped_data = std::span<u8>{std::bit_cast<u8*>(alloc_info.pMappedData), size_bytes};
        }
        is_coherent = property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
}

void Buffer::Flush(u64 offset, u64 size) {
    if (mapped_data.empty() || is_coherent) {
        return;
    }
    vmaFlushAllocation(buffer.allocator, buffer.allocation, offset, size);
}

void Buffer::Invalidate(u64 offset, u64 size) {
    if (mapped_data.empty() || is_coherent) {
        return;
    }
    vmaInvalidateAllocation(buffer.allocator, buffer.allocation, offset, size);
}

constexpr u64 WATCHES_INITIAL_RESERVE = 0x100;
constexpr u64 WATCHES_RESERVE_CHUNK = 0x100;

StreamBuffer::StreamBuffer(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler_,
                           MemoryType mem_type, u64 size_bytes)
    : Buffer{instance, 0, size_bytes, mem_type}, scheduler{scheduler_},
      non_coherent_atom_size{instance.NonCoherentAtomSize()} {
    ReserveWatches(current_watches, WATCHES_INITIAL_RESERVE);
    ReserveWatches(previous_watches, WATCHES_INITIAL_RESERVE);
    Vulkan::SetObjectName(instance.GetDevice(), Handle(), "StreamBuffer({}):{:#x}",
                          BufferTypeName(mem_type), size_bytes);
}

bool StreamBuffer::PrepareMap(u64 size, u64 alignment, bool allow_wait) {
    if (!mapped_data.empty() && !is_coherent) {
        size = Common::AlignUp(size, non_coherent_atom_size);
        alignment =
            alignment > 0 ? std::lcm(alignment, non_coherent_atom_size) : non_coherent_atom_size;
    }

    if (size > this->size_bytes) {
        return false;
    }

    if (alignment > 0) {
        offset = Common::AlignUp(offset, alignment);
    }

    if (offset + size > this->size_bytes) {
        // The buffer would overflow, save the amount of used watches and reset the state.
        invalidation_mark = current_watch_cursor;
        current_watch_cursor = 0;
        offset = 0;

        // Swap watches and reset waiting cursors.
        std::swap(previous_watches, current_watches);
        wait_cursor = 0;
        wait_bound = 0;
    }

    if (!WaitPendingOperations(offset + size, allow_wait)) {
        return false;
    }

    mapped_size = size;
    return true;
}

std::pair<u8*, u64> StreamBuffer::Map(u64 size, u64 alignment, bool allow_wait) {
    if (!PrepareMap(size, alignment, allow_wait)) {
        return {nullptr, 0};
    }
    u8* const data = mapped_data.empty() ? nullptr : mapped_data.data() + offset;
    return {data, offset};
}

void StreamBuffer::Commit() {
    if (mem_type == MemoryType::HostCached) {
        Invalidate(offset, mapped_size);
    } else {
        Flush(offset, mapped_size);
    }
    AdvanceAndWatch();
}

std::optional<u64> StreamBuffer::Reserve(u64 size, u64 alignment, bool allow_wait) {
    if (!PrepareMap(size, alignment, allow_wait)) {
        return std::nullopt;
    }
    const u64 reserved_offset = offset;
    AdvanceAndWatch();
    return reserved_offset;
}

void StreamBuffer::AdvanceAndWatch() {
    offset += mapped_size;
    const u64 tick = scheduler.CurrentTick();
    last_tick = tick;

    // Extend the last watch if it belongs to the same tick.
    if (current_watch_cursor != 0 && current_watches[current_watch_cursor - 1].tick == tick) {
        current_watches[current_watch_cursor - 1].upper_bound = offset;
        return;
    }

    if (current_watch_cursor + 1 >= current_watches.size()) {
        // Ensure that there are enough watches.
        ReserveWatches(current_watches, WATCHES_RESERVE_CHUNK);
    }

    auto& watch = current_watches[current_watch_cursor++];
    watch.upper_bound = offset;
    watch.tick = tick;
}

void StreamBuffer::ReserveWatches(std::vector<Watch>& watches, std::size_t grow_size) {
    watches.resize(watches.size() + grow_size);
}

bool StreamBuffer::WaitPendingOperations(u64 requested_upper_bound, bool allow_wait) {
    if (!invalidation_mark) {
        return true;
    }
    while (requested_upper_bound > wait_bound && wait_cursor < *invalidation_mark) {
        auto& watch = previous_watches[wait_cursor];
        if (!scheduler.IsFree(watch.tick) && !allow_wait) {
            return false;
        }
        scheduler.Wait(watch.tick);
        wait_bound = watch.upper_bound;
        ++wait_cursor;
    }
    return true;
}

} // namespace VideoCore
