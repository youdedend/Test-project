/* Sparse temporary LocalState pager for Bloodborne on Xbox Series X|S (5,120 MiB budget):
 * second-chance clock sweep, 4 KiB protection-preserving eviction, FNV-1a integrity verification,
 * and a 128 MiB mapped-window frame pool. */
#include <assert.h>
#include <fileapifromapp.h>
#include <stdlib.h>
#include <winioctl.h>

extern uint64_t bb_uwp_app_memory_usage(void);
extern uint64_t bb_uwp_app_memory_limit(void);
extern void bb_uwp_pager_thread_init(void);

static HANDLE uwp_swap_file = INVALID_HANDLE_VALUE, uwp_swap_section;
/* bbport: the page codec for the swap file (see xbox/platform/swap_codec.h and
 * xbox/tests/swap_codec_test.cpp). Guest private pages compress, and the console refused further
 * pages at 1,171 MiB - the room is fixed, so the pages go in smaller. */
#include "swap_codec.h"

static unsigned char *uwp_swap_alias, *uwp_swap_frames;
static uint64_t uwp_swap_frame_count;
static unsigned char uwp_swap_copy[16384];
static uint64_t uwp_swap_out, uwp_swap_in, uwp_swap_disk_pages, uwp_swap_views;
static uint64_t uwp_swap_hash_checks, uwp_swap_hash_errors, uwp_swap_view_evictions;
static uint64_t uwp_swap_view_limit = UINT64_C(64) * 1024 * 1024 / PAGE;
/* bbport: passes that were asked to page something out and gave nothing back. With every private
 * page pinned (the old rule, see uwp_pin_private_pages) this is what the emergency passes did at
 * the end of a session - six of them, 0 MiB each - and nothing in the log said so. */
static uint64_t uwp_swap_passes, uwp_swap_dry;
/* bbport: the disk side of the pager. `.559` paged 909 MiB out, was still going, and stopped the
 * app with "STOP: UWP swap private write ..., error 112" - ERROR_DISK_FULL. LocalState has a fixed
 * amount of room and it is shared with saves: filling it is worse than not paging, so the pager
 * keeps a budget (BB_UWP_SWAP_DISK_MB, 768 MiB by default) and treats a failed write as "this page
 * stays resident", never as a reason to stop. */
static uint64_t uwp_swap_disk_budget = UINT64_C(768) << 20;
/* bbport: the budget is a self-imposed ceiling, not a property of the device, and `.736` ran into
 * the difference: `disk pages=768 MiB of a 768 MiB budget` while the page table still held 1,209 MiB
 * of pages that passed every other test, and the pager answered `paged out 0 MiB on request` ten
 * times in a row at 5,116/5,120 MiB until the game froze. So the budget grows on demand - only
 * while an allocation is actually at stake (`runtime_memory_page_out`), in 256 MiB steps, up to a
 * ceiling. Past the ceiling, or the moment the device answers ERROR_DISK_FULL, the old behaviour
 * stands: the page stays resident and the ladder moves on. An explicit BB_UWP_SWAP_DISK_MB is a
 * hard limit and does not grow.
 *
 * The ceiling is 1.5 GiB. `.823` took the first step (768 -> 1024 MiB) during an area load and the
 * device accepted every page: `disk pages=1024 MiB of a 1024 MiB budget`, `write failures=0`,
 * `full=0`, and it bought 62 MiB in 220 ms for a 64 MiB allocation. `.559` had writes refused at
 * 1,252 MiB, so one more step is likely to fit and a third will find the edge. Past the edge the
 * write fails, `full` is set, growth stops and the page stays resident - fail-soft, no stop. If the
 * pager cannot write a page at all in a sweep (a non-disk-full error), it does not grow either. */
static uint64_t uwp_swap_disk_ceiling = UINT64_C(768) << 20;
static const uint64_t uwp_swap_disk_step = UINT64_C(256) << 20;
static int uwp_swap_disk_growable, uwp_swap_disk_raised;
static uint64_t uwp_swap_disk_full, uwp_swap_write_failures;
/* bbport: `full` used to be permanent. One refused write at 1,171 MiB (.908) turned the pager off
 * for the rest of the session, so every later request answered from an empty disk and the guest
 * died. A refused write is re-tried every 10 s: if space came back (a save written, another app
 * closed, the OS trimming) the pager uses it, and if it did not, the next attempt fails the same
 * way and costs one syscall. */
static uint64_t uwp_swap_full_tick, uwp_swap_full_rearms;
/* bbport: pages taken by an urgent sweep (one that ignored the second-chance bit). The `.649` log
 * shows why they are needed: `pageable private` was 1,757 MiB when a 141 MiB request returned 0 in
 * 3 ms - every eligible page had `recent` set by the area load, and the clock sweep only clears that
 * bit as it passes. A normal sweep is cheap and leaves hot pages alone; once an allocation has
 * already failed, the page is worth more than the fault-in it will cost later. */
static uint64_t uwp_swap_urgent_pages, uwp_swap_urgent_sweeps;
/* bbport: the swap store. Pages used to be written raw at an offset equal to their physical
 * address, which made the file exactly as big as the memory it held. `.908` ended with the device
 * refusing a write at 1,171 MiB (`write failures=1`, then `full=1` for the rest of the run) while
 * 767 MiB of guest memory was still pageable and the app was 780 MiB from its limit: the room on
 * that device is what it is, so the pages are stored compressed instead.
 *
 *   uwp_swap_slot[physical / PAGE] -> (byte offset in the file) + 1, or UWP_SWAP_SLOT_ZERO, or 0
 *   each record: 2-byte little-endian payload length, then the payload
 *   payload length == PAGE means the page was stored raw (the codec refused it)
 *
 * Records are 4 KiB-aligned so what the sparse file actually allocates is what the budget counts.
 * A page that is entirely zero is not written at all. Nothing is freed (the disk copy is the
 * backing store for a page that is out of RAM), so the arena only ever grows - which is exactly
 * what the budget and the device's free space bound. */
#define UWP_SWAP_SLOT_ZERO UINT32_C(0xFFFFFFFF)
#define UWP_SWAP_RECORD_ALIGN UINT64_C(4096)
/* bbport: the device has one shared pool of free space, and the game's own files need some of it.
 * `.1119` ran with zero write failures and the store at 1,125 MiB - and then the game's next write
 * failed with ENOSPC and aborted the process:
 *     io_file.h:187 WriteSpan: Assertion Failed! Failed to write to file, error = No space left on device
 * Filling the device was never the goal; paging is. So the store asks how much space is left before
 * it takes a new record and keeps a reserve (BB_UWP_SWAP_RESERVE_MB, 384 MiB by default) for
 * everything that is not us - saves, the pipeline cache, the dump. A refused record is
 * ERROR_DISK_FULL and is handled like any other full-disk answer: the page stays resident.
 * GetDiskFreeSpaceExFromApp is not in the UWP subset; the plain call is allowed for the app's own
 * package paths, and if it fails the reserve is skipped (the budget still bounds the file). */
static uint64_t uwp_swap_free_checked_tick, uwp_swap_free_bytes;
static uint64_t uwp_swap_reserve = UINT64_C(384) << 20;
static uint64_t uwp_swap_reserve_hits;

static uint64_t uwp_swap_free_space(void) {
    // Once a second is plenty: the pager writes at most a few MiB per pass and a stale answer only
    // means one record is taken slightly early (- it is bounded by the reserve, not by this).
    const uint64_t now = GetTickCount64();
    if (uwp_swap_free_checked_tick && now - uwp_swap_free_checked_tick < UINT64_C(1000)) {
        return uwp_swap_free_bytes;
    }
    uwp_swap_free_checked_tick = now;
    uwp_swap_free_bytes = 0;
    const char *path = getenv("BB_UWP_SWAP_FILE");
    if (path && *path) {
        char dir[1024];
        size_t length = strlen(path);
        if (length >= sizeof(dir)) length = sizeof(dir) - 1;
        memcpy(dir, path, length);
        dir[length] = 0;
        for (size_t i = length; i > 0; --i) {
            if (dir[i - 1] == '\\' || dir[i - 1] == '/') {
                dir[i] = 0;
                break;
            }
        }
        wchar_t wide[1024];
        ULARGE_INTEGER available;
        if (MultiByteToWideChar(CP_UTF8, 0, dir, -1, wide, 1024) &&
            GetDiskFreeSpaceExW(wide, &available, NULL, NULL)) {
            uwp_swap_free_bytes = available.QuadPart;
        }
    }
    return uwp_swap_free_bytes;
}

static uint32_t *uwp_swap_slot;
static size_t uwp_swap_slot_count;
static uint64_t uwp_swap_store_next;
static uint64_t uwp_swap_store_pages, uwp_swap_store_raw, uwp_swap_store_zero;
static uint64_t uwp_swap_store_payload;
static int32_t uwp_swap_lz_table[1 << UWP_LZ_HASH_BITS];
static unsigned char uwp_swap_io[PAGE + 2];
static unsigned char uwp_swap_lz[PAGE];

/* bbport: why a sweep that was asked for memory took almost nothing. `.717` printed
 * `paged out 0 MiB on request` ten times while the report still claimed 1,534 MiB of `pageable
 * private`, and nothing in the log said which test those pages were failing. These are the sweep's
 * own counters, in the same structural buckets as uwp_swap_report plus the two state ones (disk
 * budget, view guard). Reset by runtime_memory_page_out and printed only when a request comes back
 * empty, so they cost one increment per page scanned and nothing on a normal page-out. */
static uint64_t uwp_scan_pages, uwp_scan_unmapped, uwp_scan_unpageable, uwp_scan_kind,
    uwp_scan_uncommitted, uwp_scan_noview, uwp_scan_pinned, uwp_scan_shared, uwp_scan_section,
    uwp_scan_atbudget, uwp_scan_unreadable, uwp_scan_recent_cleared, uwp_scan_viewguard,
    uwp_scan_write_failed;
static void uwp_swap_scan_reset(void) {
    uwp_scan_pages = uwp_scan_unmapped = uwp_scan_unpageable = uwp_scan_kind = 0;
    uwp_scan_uncommitted = uwp_scan_noview = uwp_scan_pinned = uwp_scan_shared = 0;
    uwp_scan_section = uwp_scan_atbudget = uwp_scan_unreadable = uwp_scan_recent_cleared = 0;
    uwp_scan_viewguard = uwp_scan_write_failed = 0;
}
/* bbport: what the pager was doing when the console froze. `.717` and `.823` both end in
 * `UWP STALL DETECTED (presents stuck for 10 s)` with nothing else in the log to blame, and a page
 * sweep holds the commit lock exclusively for 7-220 ms at a time. Two stores per sweep answer it:
 * `runtime_memory_pager_state` is printed into the stall dump. */
static uint64_t uwp_swap_lock_tick, uwp_swap_lock_count;
static const char *uwp_swap_lock_what = "-";
static uint64_t uwp_swap_request_bytes, uwp_swap_request_tick;
static uint64_t uwp_swap_last_request_bytes, uwp_swap_last_request_done;
static size_t uwp_swap_cursor, uwp_swap_view_cursor;
static int uwp_swap_disabled, uwp_swap_started;

static uint64_t uwp_swap_page_out_locked(uint64_t bytes, UwpPage *exclude, int include_views,
                                        int ignore_recent);

/* Live swap views: every 4 KiB window the pager has mapped with MapViewOfFile3FromApp, as a
 * half-open [begin, end) host interval. Mapped views are guest addresses (the runtime maps the
 * PS4 address space into this process), so a pointer handed back to the guest -- or to the GPU --
 * must be inside one of them. The list is small (one entry per window, at most the window limit)
 * and is only touched by the pager under uwp_swap_view_lock. */
#define UWP_SWAP_LIVE_VIEWS (16 * 1024)
static SRWLOCK uwp_swap_view_lock = SRWLOCK_INIT;
static struct {
    uintptr_t begin;
    uintptr_t end;
} uwp_swap_live_views[UWP_SWAP_LIVE_VIEWS];
static size_t uwp_swap_live_count;
static uint64_t uwp_swap_view_guard_rejects;
static int uwp_swap_live_full;

/* Guest memory is a 40-bit address space; anything at or above 1 TiB is not a guest pointer. */
#define UWP_SWAP_GUEST_LIMIT (UINT64_C(1) << 40)

static void uwp_swap_note_view(uintptr_t address, size_t size) {
    AcquireSRWLockExclusive(&uwp_swap_view_lock);
    if (uwp_swap_live_count < UWP_SWAP_LIVE_VIEWS) {
        uwp_swap_live_views[uwp_swap_live_count].begin = address;
        uwp_swap_live_views[uwp_swap_live_count].end = address + size;
        ++uwp_swap_live_count;
    } else if (!uwp_swap_live_full) {
        uwp_swap_live_full = 1;
        printf("UWP swap: live-view registry full (%d windows); view guard fails open\n",
               (int)UWP_SWAP_LIVE_VIEWS);
    }
    ReleaseSRWLockExclusive(&uwp_swap_view_lock);
}

static void uwp_swap_forget_view(uintptr_t address, size_t size) {
    AcquireSRWLockExclusive(&uwp_swap_view_lock);
    for (size_t i = 0; i < uwp_swap_live_count; ++i) {
        if (uwp_swap_live_views[i].begin == address &&
            uwp_swap_live_views[i].end == address + size) {
            uwp_swap_live_views[i] = uwp_swap_live_views[--uwp_swap_live_count];
            break;
        }
    }
    ReleaseSRWLockExclusive(&uwp_swap_view_lock);
}

/* Is `pointer + size` inside the guest address space at all? */
static int uwp_swap_guest_ok(const void *pointer, size_t size) {
    const uintptr_t begin = (uintptr_t)pointer;
    return begin && begin < UWP_SWAP_GUEST_LIMIT && size <= UWP_SWAP_GUEST_LIMIT - begin;
}

static int uwp_swap_live_view(const void *pointer, size_t size) {
    if (!uwp_swap_guest_ok(pointer, size)) {
        return 0;
    }
    const uintptr_t begin = (uintptr_t)pointer;
    if (uwp_swap_live_full) {
        return 1;
    }
    const uintptr_t end = begin + size;
    int found = 0;
    AcquireSRWLockShared(&uwp_swap_view_lock);
    for (size_t i = 0; i < uwp_swap_live_count; ++i) {
        if (uwp_swap_live_views[i].begin <= begin && uwp_swap_live_views[i].end >= end) {
            found = 1;
            break;
        }
    }
    ReleaseSRWLockShared(&uwp_swap_view_lock);
    return found;
}

/* A freshly mapped view: it must be the guest address that was asked for and inside the 40-bit
 * guest space. Release builds clamp (log the first few, return NULL) instead of handing out a
 * stale or non-guest pointer; accepted views are registered so uwp_swap_live_view can vouch for
 * them before any later dereference. */
static void *uwp_swap_accept_view(void *view, uintptr_t expected, size_t size, uintptr_t owner) {
    if (view && (uintptr_t)view == expected && uwp_swap_guest_ok(view, size)) {
        uwp_swap_note_view((uintptr_t)view, size);
        return view;
    }
    assert(0 && "swap view mapped at an unexpected address");
    if (uwp_swap_view_guard_rejects < 8) {
        ++uwp_swap_view_guard_rejects;
        printf("UWP swap: refusing view %p (expected %#" PRIxPTR ", owner %#" PRIxPTR
               "), returning null\n",
               view, expected, owner);
    }
    return NULL;
}

/* True when every subpage of `page` that currently has a swap view is registered as live, i.e.
 * the whole-page hash and transfer in the sweep can read it without dereferencing a window that
 * was released on another thread. */
static int uwp_swap_live_page(const UwpPage *page) {
    if (!page->view_mask) {
        return 0;
    }
    for (unsigned i = 0; i < 4; ++i) {
        if ((page->view_mask & (1u << i)) &&
            !uwp_swap_live_view((void *)(page->address + i * 4096), 4096)) {
            return 0;
        }
    }
    return 1;
}

static uint64_t uwp_swap_hash(const void *data) {
    const uint64_t *words = data;
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < PAGE / sizeof(*words); ++i) {
        hash = (hash ^ words[i]) * UINT64_C(0x100000001b3);
    }
    return hash;
}

static int uwp_swap_open(void) {
    if (uwp_swap_section) return 1;
    if (uwp_swap_disabled) return 0;
    const char *path = getenv("BB_UWP_SWAP_FILE");
    if (!path || !*path) return 0;
    wchar_t wide[32768];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 32768)) return 0;
    CREATEFILE2_EXTENDED_PARAMETERS params = {0};
    params.dwSize = sizeof(params);
    params.dwFileAttributes = FILE_ATTRIBUTE_TEMPORARY;
    params.dwFileFlags = FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_RANDOM_ACCESS;
    uwp_swap_file = CreateFile2FromAppW(wide, GENERIC_READ | GENERIC_WRITE, 0, CREATE_ALWAYS, &params);
    DWORD done = 0;
    if (uwp_swap_file == INVALID_HANDLE_VALUE ||
        !DeviceIoControl(uwp_swap_file, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &done, NULL)) {
        goto failed;
    }
    uwp_swap_section = CreateFileMappingFromApp(
        INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_RESERVE, uwp_swap_view_limit * PAGE, NULL);
    if (!uwp_swap_section) goto failed;
    uwp_swap_alias = MapViewOfFile3FromApp(
        uwp_swap_section, GetCurrentProcess(), NULL, 0, uwp_swap_view_limit * PAGE, 0,
        PAGE_READWRITE, NULL, 0);
    if (!uwp_swap_alias) goto failed;
    uwp_swap_frames = calloc((size_t)uwp_swap_view_limit, 1);
    if (!uwp_swap_frames) goto failed;
    // bbport: one 32-bit record per guest physical page says where its compressed copy lives.
    // 241,664 pages for the 3.6 GiB direct pool + 1 GiB flexible span = 944 KiB of bookkeeping.
    uwp_swap_slot_count = (size_t)((POOL_SIZE + FLEX_SPAN) / PAGE);
    uwp_swap_slot = calloc(uwp_swap_slot_count, sizeof(*uwp_swap_slot));
    if (!uwp_swap_slot) goto failed;
    uwp_swap_store_next = 0;
    printf("UWP swap: compressed sparse backing ready; %" PRIu64 " MiB mapped-window limit, %zu "
           "page records\n",
           (uwp_swap_view_limit * PAGE) >> 20, uwp_swap_slot_count);
    return 1;
failed:
    printf("UWP swap: unavailable, error %lu\n", GetLastError());
    if (uwp_swap_alias) UnmapViewOfFile(uwp_swap_alias);
    if (uwp_swap_section) CloseHandle(uwp_swap_section);
    free(uwp_swap_frames);
    free(uwp_swap_slot);
    uwp_swap_slot = NULL;
    uwp_swap_alias = uwp_swap_frames = NULL;
    uwp_swap_section = NULL;
    if (uwp_swap_file != INVALID_HANDLE_VALUE) CloseHandle(uwp_swap_file);
    uwp_swap_file = INVALID_HANDLE_VALUE;
    uwp_swap_disabled = 1;
    return 0;
}

/// bbport: the last failed transfer's error, for the report (ERROR_DISK_FULL separates "the device
/// is out of space" from "the file went away").
static DWORD uwp_swap_transfer_error;

static int uwp_swap_io_at(uint64_t offset, void *data, size_t size, int write) {
    OVERLAPPED at = {0};
    DWORD done = 0;
    at.Offset = (DWORD)offset;
    at.OffsetHigh = (DWORD)(offset >> 32);
    BOOL ok = write ? WriteFile(uwp_swap_file, data, (DWORD)size, &done, &at)
                    : ReadFile(uwp_swap_file, data, (DWORD)size, &done, &at);
    if (!ok || done != size) {
        uwp_swap_transfer_error = GetLastError();
        return 0;
    }
    return 1;
}

/* bbport: all-zero pages are the cheapest saving available - a heap that was freed and zeroed is
 * still pageable, and it needs no bytes on disk at all. */
static int uwp_swap_blank(const void *data) {
    const uint64_t *words = data;
    for (size_t i = 0; i < PAGE / sizeof(*words); ++i) {
        if (words[i] != 0) return 0;
    }
    return 1;
}

/* Compress `data` into uwp_swap_io (2-byte length header first). Returns the total bytes to write,
 * or 0 when the page will not be stored here (blank / out of budget). */
static size_t uwp_swap_encode(const void *data) {
    uint16_t length;
    const size_t packed = uwp_lz_compress((const unsigned char *)data, PAGE, uwp_swap_lz, PAGE,
                                          uwp_swap_lz_table);
    if (packed == 0) {
        memcpy(uwp_swap_io + 2, data, PAGE);
        length = (uint16_t)PAGE;
        ++uwp_swap_store_raw;
    } else {
        memcpy(uwp_swap_io + 2, uwp_swap_lz, packed);
        length = (uint16_t)packed;
    }
    uwp_swap_io[0] = (unsigned char)(length & 0xFF);
    uwp_swap_io[1] = (unsigned char)(length >> 8);
    uwp_swap_store_payload += length;
    return (size_t)length + 2;
}

/* bbport: the page buffers below (uwp_swap_io, uwp_swap_lz, the LZ table) are shared, and a
 * transfer is reached from the pager, the GPU thread and any guest thread taking a commit fault.
 * Most callers already hold uwp_commit_lock, but not all of them do (uwp_promote from a guest map),
 * so the store keeps its own innermost lock: it is never held while taking uwp_commit_lock, so it
 * cannot deadlock. */
static SRWLOCK uwp_swap_io_lock = SRWLOCK_INIT;

static int uwp_swap_transfer_locked(uint64_t physical, void *data, int write) {
    const size_t key = (size_t)(physical / PAGE);
    if (!uwp_swap_slot || key >= uwp_swap_slot_count) {
        uwp_swap_transfer_error = ERROR_INVALID_PARAMETER;
        return 0;
    }
    if (!write) {
        const uint32_t slot = uwp_swap_slot[key];
        if (slot == 0) {
            uwp_swap_transfer_error = ERROR_NOT_FOUND;
            return 0;
        }
        if (slot == UWP_SWAP_SLOT_ZERO) {
            memset(data, 0, PAGE);
            return 1;
        }
        if (!uwp_swap_io_at((uint64_t)slot - 1, uwp_swap_io, 2, 0)) return 0;
        const size_t length = (size_t)uwp_swap_io[0] | ((size_t)uwp_swap_io[1] << 8);
        if (length == 0 || length > PAGE) {
            uwp_swap_transfer_error = ERROR_INVALID_DATA;
            return 0;
        }
        if (!uwp_swap_io_at((uint64_t)slot - 1 + 2, uwp_swap_io + 2, length, 0)) return 0;
        if (length == PAGE) {
            memcpy(data, uwp_swap_io + 2, PAGE);
            return 1;
        }
        if (uwp_lz_decompress(uwp_swap_io + 2, length, data, PAGE) != PAGE) {
            // Never hand a half-decoded page to the guest: the caller treats this as a failed read
            // (and the hash check would catch it anyway).
            uwp_swap_transfer_error = ERROR_INVALID_DATA;
            return 0;
        }
        return 1;
    }
    if (uwp_swap_blank(data)) {
        if (uwp_swap_slot[key] != UWP_SWAP_SLOT_ZERO) ++uwp_swap_store_zero;
        ++uwp_swap_store_pages;
        uwp_swap_slot[key] = UWP_SWAP_SLOT_ZERO;
        return 1;
    }
    const size_t total = uwp_swap_encode(data);
    uint64_t offset = 0;
    const uint32_t slot = uwp_swap_slot[key];
    if (slot != 0 && slot != UWP_SWAP_SLOT_ZERO) {
        // Rewriting a page that is already stored (a view saved twice): reuse its record when the
        // new payload fits, which keeps a session from growing the file on every cycle.
        unsigned char header[2];
        if (uwp_swap_io_at((uint64_t)slot - 1, header, sizeof(header), 0)) {
            const size_t stored = (size_t)header[0] | ((size_t)header[1] << 8);
            if (stored + 2 >= total) {
                offset = (uint64_t)slot - 1;
            }
        }
    }
    if (offset == 0) {
        const uint64_t need =
            (total + UWP_SWAP_RECORD_ALIGN - 1) / UWP_SWAP_RECORD_ALIGN * UWP_SWAP_RECORD_ALIGN;
        const uint64_t free_bytes = uwp_swap_free_space();
        if (free_bytes != 0 && free_bytes < need + uwp_swap_reserve) {
            // The game's own writes come first: a cache blob or a save that cannot be written stops
            // the process, a page that stays resident does not.
            ++uwp_swap_reserve_hits;
            uwp_swap_transfer_error = ERROR_DISK_FULL;
            return 0;
        }
        if (uwp_swap_store_next + need > uwp_swap_disk_budget) {
            // Our own budget, not the device's answer: ERROR_DISK_QUOTA_EXCEEDED so a full *budget*
            // is never mistaken for a full disk (the sweep checks it before getting here anyway).
            uwp_swap_transfer_error = ERROR_DISK_QUOTA_EXCEEDED;
            return 0;
        }
        if (offset + need > UINT32_MAX) {
            uwp_swap_transfer_error = ERROR_DISK_QUOTA_EXCEEDED;
            return 0;
        }
        offset = uwp_swap_store_next;
        uwp_swap_store_next += need;
    }
    if (offset + total > UINT32_MAX) {
        uwp_swap_transfer_error = ERROR_DISK_QUOTA_EXCEEDED;
        return 0;
    }
    if (!uwp_swap_io_at(offset, uwp_swap_io, total, 1)) return 0;
    uwp_swap_slot[key] = (uint32_t)offset + 1;
    ++uwp_swap_store_pages;
    return 1;
}

static int uwp_swap_transfer(uint64_t physical, void *data, int write) {
    AcquireSRWLockExclusive(&uwp_swap_io_lock);
    const int ok = uwp_swap_transfer_locked(physical, data, write);
    ReleaseSRWLockExclusive(&uwp_swap_io_lock);
    return ok;
}

static void uwp_swap_split(UwpPage *page) {
    if (page->split) return;
    for (unsigned i = 0; i < 3; ++i) {
        if (!VirtualFree((void *)(page->address + i * 4096), 4096,
                         MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            uwp_fail("swap placeholder split", page->address);
        }
    }
    page->split = 1;
}

static void uwp_swap_coalesce(UwpPage *page) {
    if (!page->split) return;
    if (!VirtualFree((void *)page->address, PAGE, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
        uwp_fail("swap placeholder coalesce", page->address);
    }
    page->split = 0;
}

/// Returns 1 when the window page was saved and unmapped, 0 when it could not be saved
/// (the page stays mapped; the caller must not count it as released).
static int uwp_swap_unmap_view(UwpPage *page) {
    UwpPhysical *physical = &uwp_physical[page->physical / PAGE];
    if (physical->frame) {
        DWORD old;
        for (unsigned i = 0; i < 4; ++i) {
            if ((page->view_mask & (1u << i)) && (page->protection[i] & PROT_WRITE)) {
                if (!VirtualProtectFromApp((void *)(page->address + i * 4096), 4096,
                                           PAGE_READONLY, &old)) {
                    uwp_fail("swap frame write protection", page->address);
                }
            }
        }
        unsigned char *data = uwp_swap_alias + (physical->frame - 1) * PAGE;
        physical->hash = uwp_swap_hash(data);
        physical->hash_valid = 1;
        if (!uwp_swap_transfer(page->physical, data, 1)) {
            // The window page stays where it is: a view we cannot save is a page we do not
            // unmap. Failing the process here turned a full disk into a stop (.559).
            // Our own budget running out is not a device write failure: the sweep checks it first
            // and this path exists only in case a record lands past it.
            if (uwp_swap_transfer_error == ERROR_DISK_QUOTA_EXCEEDED) {
                uwp_apply_protection(page);
                return 0;
            }
            ++uwp_swap_write_failures;
            if (uwp_swap_transfer_error == ERROR_DISK_FULL) {
                uwp_swap_disk_full = 1;
                uwp_swap_full_tick = GetTickCount64();
            }
            uwp_apply_protection(page);
            return 0;
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (page->view_mask & (1u << i)) {
            if (!UnmapViewOfFile2(GetCurrentProcess(), (void *)(page->address + i * 4096),
                                  MEM_PRESERVE_PLACEHOLDER)) {
                uwp_fail("swap view release", page->address);
            }
            uwp_swap_forget_view(page->address + i * 4096, 4096);
        }
    }
    if (page->view_mask) --uwp_swap_views;
    page->view_mask = 0;
    if (physical->frame) {
        uwp_swap_frames[physical->frame - 1] = 0;
        --uwp_swap_frame_count;
        physical->frame = 0;
    }
    page->kind = 4;
    return 1;
}

static int uwp_swap_view_room(UwpPage *exclude) {
    const size_t max_scan = uwp_count * 2;
    for (size_t n = 0; uwp_swap_frame_count >= uwp_swap_view_limit && n < max_scan; ++n) {
        if (uwp_swap_view_cursor >= uwp_count) uwp_swap_view_cursor = 0;
        UwpPage *page = &uwp_pages[uwp_swap_view_cursor++];
        if (page == exclude || !page->mapped || (page->kind != 3 && page->kind != 4)) continue;
        UwpPhysical *physical = &uwp_physical[page->physical / PAGE];
        if (physical->pinned || !physical->frame) continue;
        if (physical->recent) {
            physical->recent = 0;
            continue;
        }
        physical->hash_valid = 0;
        if (uwp_swap_unmap_view(page)) ++uwp_swap_view_evictions;
    }
    return uwp_swap_frame_count < uwp_swap_view_limit;
}

static unsigned char *uwp_swap_bytes(uint64_t offset) {
    UwpPhysical *physical = &uwp_physical[offset / PAGE];
    if (!physical->frame) {
        UwpPage *page = uwp_page(physical->owner);
        if (!uwp_swap_section || !uwp_swap_view_room(page)) {
            uwp_fail("swap frame capacity", physical->owner);
        }
        uint64_t frame = 0;
        while (frame < uwp_swap_view_limit && uwp_swap_frames[frame]) ++frame;
        if (frame == uwp_swap_view_limit) uwp_fail("swap frame bookkeeping", physical->owner);
        unsigned char *data = uwp_swap_alias + frame * PAGE;
        if (!VirtualAllocFromApp(data, PAGE, MEM_COMMIT, PAGE_READWRITE)) {
            uwp_fail("swap frame commit", physical->owner);
        }
        if (!uwp_swap_transfer(offset, data, 0)) uwp_fail("swap frame read", physical->owner);
        if (physical->hash_valid) {
            ++uwp_swap_hash_checks;
            if (uwp_swap_hash(data) != physical->hash) {
                ++uwp_swap_hash_errors;
                uwp_fail("swap integrity check", physical->owner);
            }
            physical->hash_valid = 0;
        }
        uwp_swap_frames[frame] = 1;
        ++uwp_swap_frame_count;
        physical->frame = (unsigned)frame + 1;
        uwp_swap_in += PAGE;
    }
    return uwp_swap_alias + (physical->frame - 1) * PAGE;
}

static int uwp_swap_write_backing(uint64_t offset, uint64_t within, const void *source, uint64_t size) {
    UwpPhysical *physical = &uwp_physical[offset / PAGE];
    unsigned char *data = physical->frame ? (uwp_swap_alias + (physical->frame - 1) * PAGE)
                                          : uwp_swap_copy;
    if (!physical->frame && !uwp_swap_transfer(offset, data, 0)) return 0;
    memcpy(data + within, source, (size_t)size);
    physical->hash_valid = 0;
    return physical->frame || uwp_swap_transfer(offset, data, 1);
}

static int uwp_swap_read_backing(uint64_t offset, uint64_t within, void *dest, uint64_t size) {
    UwpPhysical *physical = &uwp_physical[offset / PAGE];
    const unsigned char *data = physical->frame ? (uwp_swap_alias + (physical->frame - 1) * PAGE)
                                                : uwp_swap_copy;
    if (!physical->frame && !uwp_swap_transfer(offset, uwp_swap_copy, 0)) return 0;
    memcpy(dest, data + within, (size_t)size);
    return 1;
}

static int uwp_swap_page_in(UwpPage *page) {
    UwpPhysical *physical = &uwp_physical[page->physical / PAGE];
    uwp_swap_bytes(page->physical);
    unsigned before = page->view_mask;
    for (unsigned i = 0; i < 4; ++i) {
        if ((page->view_mask & (1u << i)) || !page->protection[i]) continue;
        void *view = NULL;
        for (unsigned attempt = 0;; ++attempt) {
            view = MapViewOfFile3FromApp(uwp_swap_section, GetCurrentProcess(),
                                         (void *)(page->address + i * 4096),
                                         (physical->frame - 1) * PAGE + i * 4096, 4096,
                                         MEM_REPLACE_PLACEHOLDER, win_prot(page->protection[i]),
                                         NULL, 0);
            if (view) {
                break;
            }
            // bbport: a page-in retry keeps the second-chance bit - evicting genuinely hot pages to
            // make room for one view costs more faults than it saves. Only an allocation that has
            // already failed (runtime_memory_page_out) is allowed to ignore it.
            if (attempt == 7 ||
                !uwp_swap_page_out_locked(UINT64_C(16) * 1024 * 1024, page, 1, 0)) {
                uwp_fail("swap page-in", page->address + i * 4096);
            }
        }
        // The mapped view address is about to become visible to the guest and the GPU: it must be
        // the guest address we asked for and it must be registered as live. A debug build asserts
        // that; a release build refuses the view (no stale or non-guest pointer is ever returned)
        // and treats the page-in as failed so the caller retries or the draw misses.
        if (uwp_swap_accept_view(view, page->address + i * 4096, 4096, page->address) == NULL) {
            UnmapViewOfFile2(GetCurrentProcess(), (void *)(page->address + i * 4096),
                             MEM_PRESERVE_PLACEHOLDER);
            return 0;
        }
        page->view_mask |= 1u << i;
    }
    if (!before && page->view_mask) {
        page->kind = 3;
        ++uwp_swap_views;
    }
    if (before != page->view_mask) ++uwp_commit_epoch;
    return before != page->view_mask;
}

static int uwp_swap_protect(UwpPage *page, unsigned index, int prot) {
    unsigned bit = 1u << index;
    uintptr_t address = page->address + index * 4096;
    if (page->view_mask & bit) {
        DWORD old;
        if (!VirtualProtectFromApp((void *)address, 4096, win_prot(prot), &old)) {
            if (!UnmapViewOfFile2(GetCurrentProcess(), (void *)address, MEM_PRESERVE_PLACEHOLDER)) {
                return 0;
            }
            uwp_swap_forget_view(address, 4096);
            page->view_mask &= ~bit;
            if (!page->view_mask) {
                page->kind = 4;
                --uwp_swap_views;
            }
            unsigned frame = uwp_physical[page->physical / PAGE].frame;
            if (prot && frame) {
                // A page whose backing frame was released meanwhile has nothing to remap: `frame`
                // is 1-based and 0 would have underflowed the section offset below (an invalid
                // MapViewOfFile3FromApp that used to abort the process after an unmap).
                void *view = MapViewOfFile3FromApp(
                    uwp_swap_section, GetCurrentProcess(), (void *)address,
                    (frame - 1) * PAGE + index * 4096, 4096, MEM_REPLACE_PLACEHOLDER,
                    win_prot(prot), NULL, 0);
                if (!view) {
                    uwp_fail("swap protection remap", address);
                }
                if (uwp_swap_accept_view(view, address, 4096, page->address) == NULL) {
                    return 0;
                }
                page->view_mask |= bit;
            }
        }
    }
    page->protection[index] = prot;
    ++uwp_commit_epoch;
    return 1;
}

static uint64_t uwp_swap_page_out_locked(uint64_t bytes, UwpPage *exclude, int include_views,
                                        int ignore_recent) {
    if (!uwp_swap_open()) return 0;
    if (bytes) ++uwp_swap_passes;
    // bbport: one refused write is not a verdict for the rest of the session (see uwp_swap_full_tick).
    if (uwp_swap_disk_full && GetTickCount64() - uwp_swap_full_tick > UINT64_C(10000)) {
        uwp_swap_disk_full = 0;
        ++uwp_swap_full_rearms;
        if (uwp_swap_full_rearms <= 8) {
            printf("UWP swap: retrying the device after a full-disk write (attempt %" PRIu64
                   ", %" PRIu64 " MiB stored)\n",
                   uwp_swap_full_rearms, uwp_swap_store_next >> 20);
        }
    }
    uint64_t released = 0;
    const size_t max_scan = uwp_count * 2;
    for (size_t scanned = 0; scanned < max_scan && released < bytes; ++scanned) {
        // bbport: a full device must not stop the walk. `kind == 3` pages are views: releasing one
        // frees RAM without needing disk at all, and `.908` showed the cost of stopping here -
        // `scanned=0` for the rest of the run, with 767 MiB still pageable and the app 780 MiB
        // from its limit. Only the private-page write below is skipped.
        if (uwp_swap_cursor >= uwp_count) uwp_swap_cursor = 0;
        UwpPage *page = &uwp_pages[uwp_swap_cursor++];
        ++uwp_scan_pages;
        if (page == exclude) continue;
        if (!page->mapped) {
            ++uwp_scan_unmapped;
            continue;
        }
        if (!page->pageable) {
            ++uwp_scan_unpageable;
            continue;
        }
        if (page->kind != 1 && page->kind != 3) {
            ++uwp_scan_kind;
            continue;
        }
        if (page->kind == 1 && !page->committed) {
            ++uwp_scan_uncommitted;
            continue;
        }
        if (page->kind == 3 && !include_views) {
            ++uwp_scan_noview;
            continue;
        }
        UwpPhysical *physical = &uwp_physical[page->physical / PAGE];
        if (physical->pinned) {
            ++uwp_scan_pinned;
            continue;
        }
        if (physical->references != 1) {
            ++uwp_scan_shared;
            continue;
        }
        if (physical->section) {
            ++uwp_scan_section;
            continue;
        }
        if (physical->recent) {
            physical->recent = 0;
            ++uwp_scan_recent_cleared;
            if (!ignore_recent) {
                continue;
            }
            ++uwp_swap_urgent_pages;
        }
        if (page->kind == 3 && !uwp_swap_live_page(page)) {
            // The window was released behind this sweep's back (a protection change or a view
            // eviction on another thread). Never dereference an address that is no longer a live
            // view: skip it and let the next sweep see the page in its new state.
            ++uwp_swap_view_guard_rejects;
            ++uwp_scan_viewguard;
            continue;
        }
        if (page->kind == 3) {
            physical->hash_valid = 0;
            if (!uwp_swap_unmap_view(page)) {
                continue; // stayed mapped: nothing was released
            }
            released += PAGE;
            uwp_swap_out += PAGE;
            continue;
        }
        if (uwp_swap_disk_full || uwp_swap_store_next >= uwp_swap_disk_budget) {
            // Out of room on the device (or at the budget): stop paging private pages. They stay
            // resident, the sweep reports "nothing to give", and the app goes on until the
            // allocation path has to degrade instead. The exception is a sweep that was started
            // because an allocation is already waiting (uwp_swap_disk_growable): there the ceiling
            // is worth another step, because the alternative is a stop.
            if (uwp_swap_disk_full || !uwp_swap_disk_growable ||
                uwp_swap_disk_budget >= uwp_swap_disk_ceiling || uwp_scan_write_failed) {
                ++uwp_scan_atbudget;
                continue;
            }
            uwp_swap_disk_budget += uwp_swap_disk_step;
            if (uwp_swap_disk_budget > uwp_swap_disk_ceiling) {
                uwp_swap_disk_budget = uwp_swap_disk_ceiling;
            }
            ++uwp_swap_disk_raised;
            if (uwp_swap_disk_raised <= 8) {
                printf("UWP swap: disk budget raised to %" PRIu64 " MiB (ceiling %" PRIu64
                       " MiB) for a waiting allocation\n",
                       uwp_swap_disk_budget >> 20, uwp_swap_disk_ceiling >> 20);
            }
        }
        int readable = 1;
        for (unsigned i = 0; i < 4; ++i) {
            if (!(page->protection[i] & PROT_READ) || (page->protection[i] & PROT_EXEC)) {
                readable = 0;
            }
        }
        if (!readable) {
            ++uwp_scan_unreadable;
            continue;
        }
        DWORD old;
        for (unsigned i = 0; i < 4; ++i) {
            if (!VirtualProtectFromApp((void *)(page->address + i * 4096), 4096,
                                       PAGE_READONLY, &old)) {
                uwp_fail("swap write protection", page->address);
            }
        }
        physical->hash = uwp_swap_hash((void *)page->address);
        physical->hash_valid = 1;
        if (!uwp_swap_transfer(page->physical, (void *)page->address, 1)) {
            // Nothing was freed yet, so the page is still complete: put its protection back and
            // leave it resident. Retried only if the error was not a full disk.
            if (uwp_swap_transfer_error == ERROR_DISK_QUOTA_EXCEEDED) {
                // The budget, not the device (see uwp_swap_transfer): keep the page resident and
                // let the caller decide whether the ceiling is worth another step.
                ++uwp_scan_atbudget;
                uwp_apply_protection(page);
                continue;
            }
            ++uwp_swap_write_failures;
            ++uwp_scan_write_failed;
            if (uwp_swap_transfer_error == ERROR_DISK_FULL) {
                uwp_swap_disk_full = 1;
                uwp_swap_full_tick = GetTickCount64();
            }
            else physical->hash_valid = 0;
            uwp_apply_protection(page);
            continue;
        }
        if (!VirtualFree((void *)page->address, PAGE, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            uwp_fail("swap private page release", page->address);
        }
        page->placed = 0;
        page->kind = 4;
        page->committed = 0;
        uwp_swap_split(page);
        if (!physical->disk) {
            physical->disk = 1;
            ++uwp_swap_disk_pages;
        }
        released += PAGE;
        uwp_swap_out += PAGE;
    }
    if (bytes && !released) ++uwp_swap_dry;
    return released;
}

/* bbport: the page table in five buckets - what the sweep could take ("resident"), and the four
 * reasons it cannot. Shared by the 5 s report and by the empty-request line below, so a run can
 * compare "the report says 1,534 MiB pageable" with "the sweep took nothing" at the same instant.
 * The caller holds uwp_commit_lock exclusively. */
static void uwp_swap_buckets(uint64_t *resident, uint64_t *pinned, uint64_t *section,
                             uint64_t *shared, uint64_t *unreadable) {
    uint64_t res = 0, pin = 0, sect = 0, sh = 0, unread = 0;
    for (size_t i = 0; i < uwp_count; ++i) {
        const UwpPage *page = &uwp_pages[i];
        if (!page->mapped || !page->pageable) continue;
        const UwpPhysical *physical = &uwp_physical[page->physical / PAGE];
        if (physical->pinned) {
            pin += PAGE;
            continue;
        }
        if (page->kind != 1 || !page->committed) continue;
        if (physical->section) {
            sect += PAGE;
            continue;
        }
        if (physical->references != 1) {
            sh += PAGE;
            continue;
        }
        int readable = 1;
        for (unsigned k = 0; k < 4; ++k) {
            if (!(page->protection[k] & PROT_READ) || (page->protection[k] & PROT_EXEC)) {
                readable = 0;
            }
        }
        if (!readable) {
            unread += PAGE;
            continue;
        }
        res += PAGE;
    }
    *resident = res;
    *pinned = pin;
    *section = sect;
    *shared = sh;
    *unreadable = unread;
}

static uint64_t uwp_swap_reclaim(uint64_t bytes, int include_views, int ignore_recent) {
    read_lock();
    AcquireSRWLockExclusive(&uwp_commit_lock);
    // bbport: published for the stall dump - how long this sweep has held the commit lock.
    uwp_swap_lock_tick = GetTickCount64();
    uwp_swap_lock_what = include_views ? "sweep+views" : "sweep";
    ++uwp_swap_lock_count;
    uint64_t result = uwp_swap_page_out_locked(bytes, NULL, include_views, ignore_recent);
    if (ignore_recent && result) ++uwp_swap_urgent_sweeps;
    uwp_swap_lock_what = "-";
    ReleaseSRWLockExclusive(&uwp_commit_lock);
    read_unlock();
    return result;
}

/* bbport: one line for the stall dump (main.cpp prints it right after `STALL DETECTED`). If a sweep
 * is holding the commit lock, its age says whether the freeze is ours; if nothing is held, the
 * freeze is elsewhere and this says so, which is worth as much. */
void runtime_memory_pager_state(void) {
    const uint64_t now = GetTickCount64();
    const char *what = uwp_swap_lock_what;
    const uint64_t held = (what && what[0] != '-') ? now - uwp_swap_lock_tick : 0;
    // A request that is still running is the interesting case; otherwise the last one and how long
    // ago it finished, which says whether the freeze started with a page-out or after it.
    const int running = uwp_swap_request_bytes != 0;
    const uint64_t asked = running ? uwp_swap_request_bytes : uwp_swap_last_request_bytes;
    const uint64_t since = running ? now - uwp_swap_request_tick
                                   : (uwp_swap_last_request_done ? now - uwp_swap_last_request_done
                                                                 : UINT64_C(0));
    printf("UWP swap: state at the stall: lock=%s held=%" PRIu64 " ms sweeps=%" PRIu64
           " request=%" PRIu64 " MiB %s %" PRIu64 " ms ago; out=%" PRIu64 " in=%" PRIu64
           " MiB disk=%" PRIu64 " MiB budget=%" PRIu64 " MiB\n",
           what, held, uwp_swap_lock_count, asked >> 20, running ? "still running for" : "finished",
           since, uwp_swap_out >> 20, uwp_swap_in >> 20, uwp_swap_store_next >> 20,
           uwp_swap_disk_budget >> 20);
    fflush(stdout);
}

uint64_t runtime_memory_page_out(uint64_t bytes) {
    // bbport: three attempts, the last two allowed to take a page the clock sweep left alone. The
    // first sweep clears the second-chance bit on everything it passes, so a second sweep of the
    // same pages usually succeeds; when even that is not enough, an allocation has already failed
    // and the page is worth more than the fault-in it costs later (.649: 141 MiB needed, 0 MiB
    // freed, 1,757 MiB reported pageable).
    // bbport: an allocation storm produced dozens of requests in a row in `.736`, each scanning up
    // to twice the page table under the exclusive commit lock (1.25 M pages, 7 ms) and each
    // returning nothing - the game froze for 10 s at 5,116/5,120 MiB doing exactly that. If the
    // last request found nothing and no page has been released since, there is nothing new to
    // find: answer at once instead of rescanning.
    uwp_swap_request_bytes = bytes;
    uwp_swap_request_tick = GetTickCount64();
    static uint64_t empty_tick;
    static int empty_blocked;
    const uint64_t now = GetTickCount64();
    if (empty_blocked && now - empty_tick < UINT64_C(500)) {
        // Short-circuited: not in flight, so the stall dump must not report it as still running.
        uwp_swap_last_request_bytes = bytes;
        uwp_swap_last_request_done = now;
        uwp_swap_request_bytes = 0;
        return 0;
    }
    uint64_t total = 0;
    uwp_swap_scan_reset();
    // An allocation is already waiting: this is the one place allowed to raise the disk budget.
    uwp_swap_disk_growable = 1;
    for (int sweep = 0; sweep < 3 && total < bytes; ++sweep) {
        const uint64_t got = uwp_swap_reclaim(bytes - total, 1, sweep > 0);
        if (!got) {
            if (sweep > 0) break;
            continue;
        }
        total += got;
    }
    uwp_swap_disk_growable = 0;
    uwp_swap_last_request_bytes = bytes;
    uwp_swap_last_request_done = GetTickCount64();
    uwp_swap_request_bytes = 0;
    if (total) {
        printf("UWP swap: paged out %" PRIu64 " MiB on request (%" PRIu64 " MiB of it urgent)\n",
               total >> 20, (uwp_swap_urgent_pages * PAGE) >> 20);
        return total;
    }
    /* bbport: the request came back empty. `.717` printed ten of these with no explanation while
     * the 5 s report still claimed 1,534 MiB of `pageable private`, so the page table is counted
     * here, in the state the sweep actually saw: one line, only when the ladder is out of moves,
     * and only the first few times so a stuck loop cannot flood the log. */
    empty_tick = now;
    // Only a request that the *disk* refused may be short-circuited: nothing in the page table can
    // change that within half a second, and rescanning it under the commit lock is what froze the
    // game in `.736`. A request that found nothing for any other reason must always rescan.
    empty_blocked = uwp_swap_disk_full || uwp_scan_atbudget > 0;
    static unsigned uwp_swap_empty_requests;
    if (uwp_swap_empty_requests < 8) {
        ++uwp_swap_empty_requests;
        uint64_t b_resident = 0, b_pinned = 0, b_section = 0, b_shared = 0, b_unreadable = 0;
        read_lock();
        AcquireSRWLockExclusive(&uwp_commit_lock);
        uwp_swap_buckets(&b_resident, &b_pinned, &b_section, &b_shared, &b_unreadable);
        ReleaseSRWLockExclusive(&uwp_commit_lock);
        read_unlock();
        printf("UWP swap: request %" PRIu64 " MiB got nothing; scanned=%" PRIu64 " unmapped=%"
               PRIu64 " unpageable=%" PRIu64 " kind=%" PRIu64 " uncommitted=%" PRIu64
               " views-off=%" PRIu64 " pinned=%" PRIu64 " shared=%" PRIu64 " section=%" PRIu64
               " at-budget=%" PRIu64 " unreadable=%" PRIu64 " view-guard=%" PRIu64
               " recent=%" PRIu64 " write-failed=%" PRIu64
               "; disk=%" PRIu64 "/%" PRIu64 " MiB full=%d write failures=%" PRIu64
               "; page table: resident=%" PRIu64 " pinned=%" PRIu64 " section=%" PRIu64
               " shared=%" PRIu64 " unreadable=%" PRIu64 " MiB\n",
               bytes >> 20, uwp_scan_pages, uwp_scan_unmapped, uwp_scan_unpageable,
               uwp_scan_kind, uwp_scan_uncommitted, uwp_scan_noview, uwp_scan_pinned,
               uwp_scan_shared, uwp_scan_section, uwp_scan_atbudget, uwp_scan_unreadable,
               uwp_scan_viewguard, uwp_scan_recent_cleared, uwp_scan_write_failed,
               (uwp_swap_disk_pages * PAGE) >> 20, uwp_swap_disk_budget >> 20,
               (int)uwp_swap_disk_full, uwp_swap_write_failures, b_resident >> 20, b_pinned >> 20,
               b_section >> 20, b_shared >> 20, b_unreadable >> 20);
    }
    return total;
}

/* bbport: the part of guest memory the pager may never page out (pinned: a page the guest's own
 * CPU code executed from or touched as WC_GARLIC). No eviction of ours can return it, so a run
 * that ends at the 5,120 MiB app limit needs this number next to the usage: it says whether the
 * wall is guest memory the game pinned or GPU allocations. Same page walk as uwp_swap_report,
 * once per report. */
uint64_t runtime_memory_pinned_bytes(void) {
    read_lock();
    AcquireSRWLockExclusive(&uwp_commit_lock);
    uint64_t pinned = 0;
    for (size_t i = 0; i < uwp_count; ++i) {
        const UwpPage *page = &uwp_pages[i];
        if (!page->mapped || !page->pageable) continue;
        if (uwp_physical[page->physical / PAGE].pinned) pinned += PAGE;
    }
    ReleaseSRWLockExclusive(&uwp_commit_lock);
    read_unlock();
    return pinned;
}

static void uwp_swap_report(void) {
    read_lock();
    AcquireSRWLockExclusive(&uwp_commit_lock);
    // bbport: why a page the pager cannot take is untouchable, in four buckets. `pinned` is the
    // policy (uwp_pin_private_pages), the other three are structure: a promoted section, a page
    // several guest mappings share (references != 1 - never paged out, its other names would see a
    // hole), and one that is executable or not readable everywhere. Without this split a run says
    // "0 MiB paged out" and gives no hint which of the four to go after.
    uint64_t resident = 0, pinned = 0, section = 0, shared = 0, unreadable = 0;
    uwp_swap_buckets(&resident, &pinned, &section, &shared, &unreadable);
    uint64_t out = uwp_swap_out, in = uwp_swap_in;
    // bbport: `disk` is what the file actually occupies now, not pages * 16 KiB - the store
    // compresses, elides blank pages and aligns records, so the two differ by the whole point.
    uint64_t disk = uwp_swap_store_next / PAGE, views = uwp_swap_views;
    uint64_t checks = uwp_swap_hash_checks, errors = uwp_swap_hash_errors;
    uint64_t evictions = uwp_swap_view_evictions;
    ReleaseSRWLockExclusive(&uwp_commit_lock);
    read_unlock();
    const char *profile = getenv("BB_IO_STATS");
    // bbport: also print when the pinned part moved by 64 MiB or more. It used to print only
    // after the pager had paged something out, so a run in which nothing was pageable - every
    // private page pinned - said nothing at all while the unpageable part took the budget.
    const uint64_t pinned_mb = pinned >> 20;
    static uint64_t last_pinned_mb;
    const uint64_t moved = pinned_mb > last_pinned_mb ? pinned_mb - last_pinned_mb
                                                      : last_pinned_mb - pinned_mb;
    if (!out && !uwp_swap_dry && moved < 64 && !(profile && *profile == '1')) return;
    last_pinned_mb = pinned_mb;
    printf("UWP swap: out=%" PRIu64 " in=%" PRIu64 " disk=%" PRIu64 " views=%" PRIu64
           " MiB; checks=%" PRIu64 " corrupt=%" PRIu64 " window evictions=%" PRIu64 "\n",
           out >> 20, in >> 20, (disk * PAGE) >> 20, (views * PAGE) >> 20,
           checks, errors, evictions);
    printf("UWP swap: pageable private=%" PRIu64 " pinned=%" PRIu64
           " MiB; page-out passes=%" PRIu64 " with nothing to give=%" PRIu64 "\n",
           resident >> 20, pinned_mb, uwp_swap_passes, uwp_swap_dry);
    printf("UWP swap: not paged - pinned policy=%" PRIu64 " section=%" PRIu64
           " shared=%" PRIu64 " exec/unreadable=%" PRIu64 " MiB\n",
           pinned_mb, section >> 20, shared >> 20, unreadable >> 20);
    // What the disk half is doing, and whether it is full: a page-out that fails leaves the page
    // resident, so this is the difference between "nothing to page" and "nowhere to put it".
    printf("UWP swap: disk pages=%" PRIu64 " MiB of a %" PRIu64 " MiB budget (ceiling %" PRIu64
           " MiB, raised %" PRIu64 "); %" PRIu64 " pages stored (%" PRIu64 " raw, %" PRIu64
           " blank, %" PRIu64 " MiB of payload, %" PRIu64 "%% of raw); write failures=%" PRIu64
           "; urgent sweeps=%" PRIu64 " (%" PRIu64 " MiB)%s\n",
           (disk * PAGE) >> 20, uwp_swap_disk_budget >> 20, uwp_swap_disk_ceiling >> 20,
           uwp_swap_disk_raised, uwp_swap_store_pages, uwp_swap_store_raw, uwp_swap_store_zero,
           uwp_swap_store_payload >> 20,
           uwp_swap_store_pages ? (uwp_swap_store_payload * 100) / (uwp_swap_store_pages * PAGE)
                                : UINT64_C(0),
           uwp_swap_write_failures,
           uwp_swap_urgent_sweeps, (uwp_swap_urgent_pages * PAGE) >> 20,
           uwp_swap_disk_full ? " (device full: private pages stay resident)" : "");
    // How much of the device is left, so the next log answers "could we have taken more?" without a
    // second run: `free` already excludes the reserve this port keeps for the game's own files.
    const uint64_t free_now = uwp_swap_free_space();
    if (free_now != 0 || uwp_swap_reserve_hits) {
        printf("UWP swap: device free=%" PRIu64 " MiB (reserve %" PRIu64
               " MiB kept for the game's own writes, %" PRIu64 " record(s) refused by it)\n",
               free_now >> 20, uwp_swap_reserve >> 20, uwp_swap_reserve_hits);
    }
}

static void *uwp_pager(void *unused) {
    (void)unused;
    bb_uwp_pager_thread_init();
    uint64_t reported = 0;
    int draining = 0;
    for (;;) {
        uint64_t limit = bb_uwp_app_memory_limit();
        uint64_t used = bb_uwp_app_memory_usage();
        uint64_t out = 0;
        if (limit > (UINT64_C(1024) << 20)) {
            /* cosmo/gow3_xbox guest_swap.inc: page out only inside the last 384 MiB of the app
             * budget and stop again at 512 MiB of headroom. Starting at 896 MiB meant paging
             * during the loading spikes themselves, and the urgent pass recycled shared-frame
             * window pages (include_views = 1) that the GPU still read: every pass then freed
             * less private commit than it churned, and the guest threads it stalled were the
             * ones loading the next area. A 2 MiB pass keeps the copy thread cheap. */
            // bbport: start earlier than the 384 MiB band. The last two runs both died in an
            // area load that added ~300-400 MiB in a second while the pager was still working at
            // 2 MiB a pass; starting at 640 MiB of headroom gives it that time, and the disk
            // budget bounds what it can take either way.
            if (used > limit - (UINT64_C(640) << 20)) draining = 1;
            if (used <= limit - (UINT64_C(768) << 20)) draining = 0;
            if (draining) out = uwp_swap_reclaim(UINT64_C(2) << 20, 0, 0);
        }
        uint64_t now = GetTickCount64();
        if (now - reported >= 5000) {
            uwp_swap_report();
            reported = now;
        }
        Sleep(out ? 1 : 40);
    }
    return NULL;
}

void runtime_memory_start_paging(void) {
    if (uwp_swap_started) return;
    // bbport: room the pager may take in LocalState. 768 MiB by default: enough to cover the gap
    // the last runs died in, small enough to leave the device's storage for saves.
    {
        // Default: start at 768 MiB and let a waiting allocation raise it, 256 MiB at a time, to
        // 1.5 GiB. An explicit BB_UWP_SWAP_DISK_MB is taken as a hard limit and never grows.
        uwp_swap_disk_ceiling = UINT64_C(1536) << 20;
        const char *budget = getenv("BB_UWP_SWAP_DISK_MB");
        const unsigned long long mb = budget ? strtoull(budget, NULL, 10) : 0;
        if (mb >= 64 && mb <= 8192) {
            uwp_swap_disk_budget = mb << 20;
            uwp_swap_disk_ceiling = uwp_swap_disk_budget;
        }
        const char *reserve = getenv("BB_UWP_SWAP_RESERVE_MB");
        const unsigned long long reserve_mb = reserve ? strtoull(reserve, NULL, 10) : 0;
        if (reserve_mb <= 4096) uwp_swap_reserve = reserve_mb << 20;
    }
    const char *enabled = getenv("BB_UWP_SWAP");
    if (enabled && *enabled == '0') {
        uwp_swap_disabled = 1;
        puts("UWP swap: disabled by BB_UWP_SWAP=0");
        return;
    }
    runtime_memory_reserve();
    if (!uwp_swap_open()) {
        puts("UWP swap: pager could not start");
        return;
    }
    pthread_t thread;
    if (pthread_create(&thread, NULL, uwp_pager, NULL)) uwp_fail("pager thread", 0);
    pthread_detach(thread);
    uwp_swap_started = 1;
    printf("UWP swap: pager active, app budget=%" PRIu64 " MiB, pressure headroom=640 MiB; "
           "private pages %s; disk budget=%" PRIu64 " MiB (ceiling %" PRIu64 " MiB, reserve %"
           PRIu64 " MiB for the game's own writes)\n",
           bb_uwp_app_memory_limit() >> 20,
           uwp_pin_private_pages() ? "pinned by BB_UWP_PIN_PRIVATE" : "pageable",
           uwp_swap_disk_budget >> 20, uwp_swap_disk_ceiling >> 20, uwp_swap_reserve >> 20);
}
