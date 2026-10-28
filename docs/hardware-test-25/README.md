# Console run 11 — `.1119` (2026-10-27): compression works, and the swap took the game's disk

Logs: `loader.log.txt` (1,283 lines), `errors.log.txt` (80), `dozen.log.txt` (2). Build identity:
`UWP swap: compressed sparse backing ready; 64 MiB mapped-window limit, 389120 page records`.

## The codec does what it was written for

Progression through the run, from the report line:

| stored | raw | blank | payload | % of raw |
|---|---|---|---|---|
| 15,755 | 36 | 2,887 | 1 MiB | 0% (early, mostly untouched pages) |
| 47,390 | 85 | 19,683 | 85 MiB | 11% |
| 61,261 | 260 | 19,820 | 233 MiB | 24% |
| 90,799 | 676 | 19,820 | 522 MiB | 36% |
| **125,940** | **1,492** | **20,604** | **890 MiB** | **45%** |

99% of non-blank pages compressed; **1,125 MiB of device held ~2,500 MiB of guest pages**, and
20,604 pages needed no bytes at all. `pageable private=0` at the end: everything pageable had been
paged out. **Present 941**, against 832 in `.908` and 792 in `.823` — the furthest this port has
reached in this area.

No `FATAL`, no `ERROR_DISK_FULL` from the pager: `write failures=0` for the whole run, budget raised
twice (768 → 1,024 → 1,280 MiB).

## The pager was not the stall — the diagnostic says so

```
UWP STALL DETECTED (presents stuck at 939 for 10 s):
UWP swap: state at the stall: lock=sweep held=0 ms sweeps=397 request=64 MiB finished 16 ms ago
UWP STALL DETECTED (presents stuck at 941 for 10 s):
UWP swap: state at the stall: lock=sweep held=0 ms sweeps=637 request=32 MiB finished 609 ms ago
```

`held=0 ms` twice: no sweep was holding `uwp_commit_lock`. The 10 s freeze is upstream of the pager —
an area load with the working set over RAM (0.6 FPS at the end, `Frame rate: 0.6 FPS`).

## What ended it: the game's own write, out of space

```
GPU [Debug] <Critical> io_file.h:187 WriteSpan: Assertion Failed!
Failed to write to file, error = No space left on device
STOP: GPU library assertion failed (see GPU log above)
```

The swap file had grown to 1,125 MiB on a device with ~1.2 GiB to spare, and the pipeline-cache
flush lost the race. The pager did not fail — it did its job and the *game's* next write could not.

## Fixes in the next build

| What | Where |
|---|---|
| **A reserve for the game's own files**: before a new record is taken, the store asks the device how much space is left (`GetDiskFreeSpaceExW` on the swap file's directory, cached for 1 s) and refuses to come closer than **384 MiB** (`BB_UWP_SWAP_RESERVE_MB`). A refused record is `ERROR_DISK_FULL`, handled like any other: the page stays resident | `guest_swap.inc:76-125, 445-455` |
| The report prints `device free=… MiB (reserve … MiB kept for the game's own writes, N record(s) refused by it)`, so the next log answers "could we have taken more?" | `guest_swap.inc:1100-1112` |
| A cache write that hits `ENOSPC` no longer asserts: the short write is reported to the caller (which already removes the partial file and recompiles next time), and the assertion stays for every other error | `gpu/shadps4/common/io_file.h:186-208` |

Nothing else about the ladder changed: paging, image reclaim and the ceiling work as before.

## Still open

- The 10 s stall and the 0.6 FPS at the end are **not** the pager's lock. It is an area load whose
  working set does not fit in 5 GiB. The approved ladder has one rung left that has not been pulled:
  a lower output resolution / frame-generation setting. That is a settings default, so it needs a
  decision before it ships.
- `disk pages=1125 MiB` was reached with `free` never measured; the next run prints it.
