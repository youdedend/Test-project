// SPDX-License-Identifier: GPL-2.0-or-later
// Overflow-safe admission arithmetic. Used for diagnostics only until callers can safely abandon
// a DeviceLocal buffer request without handing the renderer a null/dangling buffer.
#pragma once
#include <cstdint>
#include <limits>

namespace BbArenaAdmission {
inline constexpr std::uint64_t kMinimum = std::uint64_t{16} << 20;
inline constexpr std::uint64_t kHeadroom = std::uint64_t{250} << 20;
inline constexpr std::uint64_t kLiveCap = std::uint64_t{768} << 20;

inline bool BudgetFits(std::uint64_t usage, std::uint64_t limit, std::uint64_t bytes,
                       std::uint64_t headroom = kHeadroom) {
    if (!limit) return true; // unknown: report separately, do not invent a ceiling
    if (usage > limit || headroom > limit - usage) return false;
    return bytes <= limit - usage - headroom;
}

inline std::uint64_t ProjectedLive(std::uint64_t live, std::uint64_t replaced,
                                   std::uint64_t incoming) {
    const std::uint64_t remaining = live > replaced ? live - replaced : 0;
    if (incoming > std::numeric_limits<std::uint64_t>::max() - remaining)
        return std::numeric_limits<std::uint64_t>::max();
    return remaining + incoming;
}

inline bool CapFits(std::uint64_t live, std::uint64_t replaced, std::uint64_t incoming,
                    std::uint64_t cap = kLiveCap) {
    return cap == 0 || ProjectedLive(live, replaced, incoming) <= cap;
}
} // namespace BbArenaAdmission
