// SPDX-License-Identifier: GPL-2.0-or-later
#include "arena_admission_policy.h"
#include <cassert>
#include <cstdio>
#include <limits>

int main() {
    using namespace BbArenaAdmission;
    constexpr std::uint64_t M = 1ull << 20;
    assert(BudgetFits(4800 * M, 5120 * M, 70 * M));
    assert(!BudgetFits(4800 * M, 5120 * M, 70 * M + 1));
    assert(!BudgetFits(5121 * M, 5120 * M, 16 * M));
    assert(BudgetFits(0, 0, std::numeric_limits<std::uint64_t>::max()));
    assert(!BudgetFits(std::numeric_limits<std::uint64_t>::max() - 10,
                       std::numeric_limits<std::uint64_t>::max(), 16 * M));
    // The live cap counts the replacement only once; the budget check counts the entire new
    // allocation in addition to current usage, including all not-yet-retired old arenas.
    assert(ProjectedLive(700 * M, 300 * M, 360 * M) == 760 * M);
    assert(CapFits(700 * M, 300 * M, 360 * M));
    assert(!CapFits(700 * M, 300 * M, 369 * M));
    assert(CapFits(800 * M, 0, 800 * M, 0));
    assert(ProjectedLive(std::numeric_limits<std::uint64_t>::max(), 0, 1) ==
           std::numeric_limits<std::uint64_t>::max());
    std::puts("arena admission arithmetic: all checks passed");
}
