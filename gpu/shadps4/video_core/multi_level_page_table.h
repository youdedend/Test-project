// SPDX-FileCopyrightText: 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstdio>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/assert.h"
#include "common/object_pool.h"
#include "common/types.h"

namespace VideoCore {

template <class Traits>
class MultiLevelPageTable final {
    using Entry = typename Traits::Entry;

    static constexpr size_t AddressSpaceBits = Traits::AddressSpaceBits;
    static constexpr size_t FirstLevelBits = Traits::FirstLevelBits;
    static constexpr size_t PageBits = Traits::PageBits;
    static constexpr size_t FirstLevelShift = AddressSpaceBits - FirstLevelBits;
    static constexpr size_t SecondLevelBits = FirstLevelShift - PageBits;
    static constexpr size_t NumEntriesPerL1Page = 1ULL << SecondLevelBits;

    using L1Page = std::array<Entry, NumEntriesPerL1Page>;

public:
    explicit MultiLevelPageTable() : first_level_map{1ULL << FirstLevelBits, nullptr} {}

    ~MultiLevelPageTable() noexcept = default;

    [[nodiscard]] Entry* find(size_t page) {
        const size_t l1_page = page >> SecondLevelBits;
        if (l1_page >= first_level_map.size()) {
            return nullptr;
        }
        const size_t l2_page = page & (NumEntriesPerL1Page - 1);
        if (!first_level_map[l1_page]) {
            return nullptr;
        }
        return &(*first_level_map[l1_page])[l2_page];
    }

    [[nodiscard]] const Entry* find(size_t page) const {
        const size_t l1_page = page >> SecondLevelBits;
        if (l1_page >= first_level_map.size()) {
            return nullptr;
        }
        const size_t l2_page = page & (NumEntriesPerL1Page - 1);
        if (!first_level_map[l1_page]) {
            return nullptr;
        }
        return &(*first_level_map[l1_page])[l2_page];
    }

    [[nodiscard]] const Entry& operator[](size_t page) const {
        const size_t l1_page = page >> SecondLevelBits;
        if (l1_page >= first_level_map.size()) [[unlikely]] {
            return SinkEntry(page);
        }
        const size_t l2_page = page & (NumEntriesPerL1Page - 1);
        if (!first_level_map[l1_page]) {
            first_level_map[l1_page] = page_alloc.Create();
        }
        return (*first_level_map[l1_page])[l2_page];
    }

    [[nodiscard]] Entry& operator[](size_t page) {
        const size_t l1_page = page >> SecondLevelBits;
        if (l1_page >= first_level_map.size()) [[unlikely]] {
            return SinkEntry(page);
        }
        const size_t l2_page = page & (NumEntriesPerL1Page - 1);
        if (!first_level_map[l1_page]) {
            first_level_map[l1_page] = page_alloc.Create();
        }
        return (*first_level_map[l1_page])[l2_page];
    }

private:
    /// Out-of-range index: the ASSERT_MSG above is compiled out in release builds, and reading
    /// `first_level_map[l1_page]` out of bounds is exactly the fault Hardware Test #11 reported
    /// (read of 0x20500987170, first_level_map base + 0x2878A160 = a far out-of-range slot).
    /// A guest address beyond the 1 TiB address space produces such an index; degrade to a
    /// shared sink page and log the first few offenders instead of faulting the GPU thread.
    [[nodiscard]] static Entry& SinkEntry(size_t page) {
        static Entry sink{};
        static std::atomic<u64> reports{0};
        const u64 count = reports.fetch_add(1, std::memory_order_relaxed);
        if (count < 8) {
            std::printf("UWP page table: out-of-range page index %#zx (%#zx entries), skipping\n",
                        page, size_t{1} << FirstLevelBits);
            std::fflush(stdout);
        }
        return sink;
    }

    std::vector<L1Page*> first_level_map{};
    Common::ObjectPool<L1Page> page_alloc;
};

} // namespace VideoCore
