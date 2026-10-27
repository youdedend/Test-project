# Console log — build `.559`: the pin fix works, the pager fills the device and stops on disk-full

Console build `1.0.26300.559` (release `xbox-alpha-1.0.26300.0559`, target `817f291`). Sent
2026-10-27, right after the crash and before relaunching. First build with the new pin rule.

| File | What it is |
|---|---|
| `loader.log.txt` | 1,008 lines: the session, 20+ telemetry samples, the pager reports, a 10 s stall dump |
| `errors.log.txt` | GPU/`stderr` side, ending in `STOP: UWP swap private write …, error 112` |

## The pin fix did what it was meant to

| Measurement | `.534` (before) | `.559` (this run) |
|---|---|---|
| `guest pinned` | 0 → 1,355 MiB | **0 MiB the whole session** |
| pager's pageable set | ~400 MiB, flat | **989 MiB**, reported openly |
| page-out passes / with nothing to give | 6 emergency passes, all 0 MiB | **237 / 0** |
| swapped out | 0 MiB | **908 → 1,016 MiB** (checks 274 → 660, `corrupt=0`) |
| session | died at `presents=829` on a 16 MiB allocation | **same presents, different reason** (below) |

`UWP memory telemetry` shows `guest pinned=0 MiB` on every sample and `UWP swap: out=908 in=3
disk=907 … checks=274 corrupt=0` — the pager was doing its job for the first time.

## The new stop: the device ran out of disk

```
UWP swap: out=1016 in=9 disk=1015 views=9 MiB; checks=660 corrupt=0 window evictions=0
STOP: UWP swap private write at 00000010ACD30000, error 112        <- ERROR_DISK_FULL
```

The sparse swap file in `LocalState` reached ~1 GiB and the volume had no more room, and the pager
treated a failed write as fatal. Notes:

- There was a `UWP STALL DETECTED (presents stuck at 829 for 10 s)` right before it, and during the
  stall the pager still paged 108 MiB out — the allocation-retry path pages out synchronously while
  the GPU thread waits, which is where the second of silence came from.
- `presents` never advanced past 829, the same point `.534` died at. So the area load needs more
  than 5,120 MiB, and every mechanism is now doing its best: memory that could be handed back was
  handed back (1 GiB to disk), and there is nothing else to give.
- New warning also seen once: `ProcessGraphics: PM4 header 0x00000002 (type 0) at +0x3a20: rest of
  the buffer dropped` — unrelated to memory, worth remembering.

## What this run proves

1. The pin rule was the reason the pager had no work (§2j). That part is now measured, not assumed.
2. LocalState is not free space: ~1 GiB is what the volume had, and saves live there too. A pager
   that fills it is worse than one that does not page, so it now keeps a budget and fails soft (§2k).
3. The remaining gap is real capacity: the scene at `presents=829` needs more than 5,120 MiB.
