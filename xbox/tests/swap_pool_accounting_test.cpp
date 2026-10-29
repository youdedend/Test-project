// SPDX-License-Identifier: GPL-2.0-or-later
#include "swap_pool_accounting.h"
#include <cassert>
#include <cstdio>

int main() {
    unsigned char map[2]{};
    const uint64_t p = BB_POOL_PAGE_SIZE;
    assert(bb_pool_mark_pages(map, 2 * p, 3 * p, 1) == 3 * p);
    // A failed decommit leaves the bits set; reuse cannot count the same physical pages twice.
    assert(bb_pool_mark_pages(map, 2 * p, 3 * p, 1) == 0);
    assert(bb_pool_mark_pages(map, 3 * p, 2 * p, 0) == 2 * p);
    assert(bb_pool_mark_pages(map, 4 * p, 3 * p, 1) == 3 * p);
    assert(bb_pool_mark_pages(map, 0, 16 * p, 0) == 4 * p);
    assert(bb_pool_mark_pages(map, 0, 16 * p, 0) == 0);
    assert(bb_pool_mark_pages(map, 7 * p, 3 * p, 1) == 3 * p); // crosses a bitmap byte
    assert(bb_pool_net_saved(16 * p, 6 * p) == 10 * p);
    assert(bb_pool_net_saved(3 * p, 6 * p) == 0);
    std::puts("swap pool accounting: all checks passed");
}
