# Master plan, BUILD 1: storage, pager, diagnostics

Three builds, flag-gated, each measured. This document is BUILD 1 only. Nothing here has been
measured on a console yet; the numbers quoted are the ones the code will print, and the checks that
passed locally are named.

Evidence this build answers to: `0717`/`0736`/`819` all reached 5,116/5,120 MiB at world load and
stalled. In `819` the swap budget was capped to 483 MiB because the LocalState volume had 867 MiB
free (the console has 30 GB elsewhere), the volume filled, 6,828 writes were refused, and presents
stopped at 877. In `0717` the emergency reclaim found nothing to free.

## Item by item

### 1. Every volume, and what LocalState spends it on — `xbox/platform/storage_report.cpp`

At startup, after the run header:

```
UWP storage: LocalState is on Q: (...)
UWP storage: C: total=... free=... read-only to this app
UWP storage: Q: total=... free=867 MiB [LocalState] writable
UWP storage: largest writable volume other than LocalState's is ...
UWP storage: LocalState\Prepared = ... in N file(s)
UWP storage: LocalState total = ... of ... on the volume
UWP STORAGE WARNING: 867 MiB free on Q:, below the 1.0 GiB this port wants ...
```

* Each volume is probed, not inferred: `GetDiskFreeSpaceExW` for the numbers, then a 64 KiB file
  written and deleted (`FILE_FLAG_DELETE_ON_CLOSE`, so a crash cannot litter the drive). A UWP app may
  read most volumes and write almost none; "30 GB free on D:" is worthless if the swap file cannot
  live there, and the log now says which is which. This is also the answer to item 3's "evaluate a
  larger volume": the report names the largest *writable* volume that is not LocalState's.
* Every top-level folder of LocalState is sized (walk capped at 200,000 entries; a capped walk is
  labelled as a lower bound rather than reported as the answer).
* Visible warning: below 1 GiB free the player gets a dialog before the game starts
  (`BB_UWP_STORAGE_DIALOG=0` disables it) and the pager's cap is no longer a surprise. The check
  repeats on the heartbeat, so a volume that fills *during* a session says so too.

### 2. Zero pages cost no budget — `guest_swap.inc`, the at-budget branch

An all-zero page needs no record: the slot is the `ZERO` sentinel and the reader turns it into a
`memset`. The sweep, however, skipped every private page *before* the transfer could discover that -
which is why `819` kept every zero page in RAM. The at-budget branch now tests the page (same
protection rule as the write path: read-only-or-better, not executable) and releases it with no disk
cost. New counters: `uwp_swap_zero_paged` (cumulative, printed in the memory table) and
`uwp_scan_zero_paged` (per pass). Blank pages are also no longer counted as disk pages, which is why
the old report's "disk pages" disagreed with its own file size.

Measured as: `zero pages paged out with no record=N MiB` in the memory table.

### 3. In-memory compressed pool, the second tier — `guest_swap.inc`

When the device refuses a record (budget at its cap, volume full, or the reserve for the game's own
files in the way), the same compressed payload goes into a fixed RAM reservation instead:
16 KiB page out, ~6 KiB blob in, so the app budget gets back the difference. First-fit arena with
coalescing, blobs committed and decommitted as they come and go, descriptors a flat array (nothing
allocates on the page-out or page-in path), all under the existing `uwp_swap_io_lock`. A blob is the
page's only copy, so it is never dropped to make room; when the pool is full the pager behaves
exactly as before. `BB_UWP_SWAP_POOL_MB` (default 192, `0` disables) bounds it.

Also: with a pool the "disk blocked" early return in the sweep no longer short-circuits, because a
sweep with pool room can still release memory. That early return was `819`'s no-op sweeps.

Measured as, every 5 s and in the memory table:

```
UWP swap pool: N pages = X MiB of guest memory in Y MiB committed (capacity C MiB, peak P MiB)
             = Z MiB kept out of the app budget; N page-ins, R refused, D decommit failures
```

`Z` is the number to compare against `at-budget` in the same table: memory the disk could not take
and RAM could.

### 4. The skip counters, and the memory table's arithmetic

* `uwp_scan_atbudget` is a **page** count and `uwp_scan_atbudget_bytes` its byte total; both reset
  every pass (the `.819` run printed `at-budget=206460 (164162 MiB)` because the byte counter did not
  reset with its denominator). They are now printed next to two counters that say where the page
  actually went: the pool (taken) and `at-budget` (stayed resident).
* `unaccounted` was negative because the line subtracted overlapping views: attempt 2 computed
  `resident + (device - resident)`, which is just `device`, and `device` covers roughly the whole app,
  so `usage - device` was ≤ 0 by construction. Now the floor is `max(resident, device)`, the overlap
  is printed explicitly, and the label says which is which.

### 5. Heartbeat

Already its own thread on a 5 s / 1 s-near-limit cadence, writing unbuffered. Added this build: GPU
**fence age** (`fence_stalled_ms`: how long the completed fence has stood still while work is
outstanding - the number that separates "the GPU is busy" from "the GPU stopped"), and the pool and
zero-page counters. The line now carries app usage/limit, level, presents, fence
submitted/completed/age, swap used/budget, arena live, hard faults and working set.

### 6. Per-thread operations, big allocations, slow waits

`bbgpu_thread_op` already tags `bb:VkRecorder`-side command recording, PM4 processing, fence waits and
the pager; this build adds **guest main**. Allocations ≥ 32 MiB and waits ≥ 250 ms are already logged
(`bbgpu_note_alloc`, the scheduler's fence-wait line). All three appear in the stall dump.

### 7. Log header, per-run file, ring buffer, breadcrumbs

Already shipped in `.930`: build/commit/branch/marks/resolution/flags header, one timestamped file
per run, a 200-line breadcrumb ring dumped on fault and on `std::terminate`, synchronous
suspend/resume and memory-limit-change lines. This build adds the two new switches to the flags line
so an A/B run says which tier was armed.

## What is flag-gated

| Setting | Default | Effect |
|---|---|---|
| `BB_UWP_SWAP_POOL_MB` | 192 | size of the in-memory tier; 0 disables it |
| `BB_UWP_SWAP_BLANK` | 1 | all-zero pages need no record (item 2) |
| `BB_UWP_STORAGE_DIALOG` | 1 | the visible low-storage warning |

All three are printed in the `UWP flags:` line of every run.

## Tests

* `xbox/tests/storage_report_test.cpp` (new, in `xbox.yml`): size formatting, the low-storage
  verdict, "biggest **writable** volume, not the biggest", and the folder walk including that a
  capped walk reports itself as capped. Passes locally (g++, Linux).
* All 7 existing console-side unit tests still compile and pass locally.
* `graphics_toggles_test` still passes.

## What needs the console

That the pool actually saves what it says, that a 192 MiB pool is the right size for this game, and
that releasing zero pages at the budget does not cost more in page-ins than it saves in RAM. The
run to ask for: reach the world load with the pager at its cap and compare `at-budget` (MiB that
stayed resident) against `pool ... saved` (MiB the pool returned) in the same memory table. The
`.819` run's `at-budget` was 1,171 MiB with nothing recovering it; if the pool works, that number
falls and the freed amount appears on the pool line.
