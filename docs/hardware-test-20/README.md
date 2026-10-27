# Console log — build `.649`: disk-full fixed, the ladder reports "0 MiB paged out"

Console build `1.0.26300.649` (release `xbox-alpha-1.0.26300.0649`, target `8219ff7`). Start line
carries `disk budget=768 MiB`, so this is the build with the fail-soft page-out **and** the ladder.

| File | What it is |
|---|---|
| `loader.log.txt` | 837 lines |
| `errors.log.txt` | ends in `buffer.cpp:235 Create: Assertion Failed!` |

## What is fixed

- **No `ERROR_DISK_FULL`.** `disk pages=185 MiB of a 768 MiB budget; write failures=0` on every
  report: the budget held and no write ever failed.
- **No 10 s stall** (`grep -c STALL` = 0). Starting the pager at 640 MiB of headroom moved the work
  ahead of the spike: the pre-allocation passes cost 7 / 19 / 109 ms.
- The session ran 6 presents longer (861 vs 815) and `guest pinned` stayed 0 MiB.
- The ladder ran and printed itself rather than stopping silently.

## The new numbers

```
UWP swap: pageable private=1757 pinned=0 MiB; page-out passes=93 with nothing to give=0
UWP memory emergency (allocation failed): need 141 MiB; paged out 0 MiB in 3 ms,
    freed 0 MiB of images in 0 ms (usage now 5101/5120 MiB)
[uwp-vk] FATAL vmaCreateBuffer(65536 KiB, type=DeviceLocal) -> -2; Xbox AppMemory 5101/5120 MiB
```

`pageable private` was **1,757 MiB** and a 141 MiB request still returned **0 MiB in 3 ms**. That is
the second-chance bit: the clock sweep sets `recent` on every page it passes and only clears it on
the *next* pass, and during an area load every eligible page had been touched since the last sweep.
The background pager's 93 passes produced exactly 2 MiB each - the same limit, at 2 MiB a pass.

Meanwhile the texture cache's emergency reclaim also returned 0 MiB, with no way to tell which keep
rule was responsible (`.649` only had the total).

Both are addressed in the next build: urgent sweeps that ignore the second-chance bit when an
allocation has already failed, and a one-line breakdown of the image reclaim.
