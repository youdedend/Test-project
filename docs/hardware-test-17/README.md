# Console log — build `.534`: the pinned-growth ramp, and the area load that tipped it

Console build `1.0.26300.534` (release `xbox-alpha-1.0.26300.0534`, target `b7fff3c`). Sent
2026-10-27, right after the crash and before relaunching. First build whose telemetry keeps printing
near the limit and shows the delta since the last sample.

| File | What it is |
|---|---|
| `loader.log.txt` | 820 lines: 20 telemetry samples with deltas, the arena burst, the headroom passes |
| `errors.log.txt` | GPU/`stderr` side, ending in the `buffer.cpp:189` assertion |

## The ramp (the part earlier builds hid)

| presents | usage | guest pinned | change |
|---|---|---|---|
| 250 | 2,286 | 235 | +279 |
| 564 | 4,401 | 1,079 | +555 |
| 652 | 4,566 | 1,265 | +108 |
| 770 | 4,634 | 1,341 | +11 |
| 793 | 4,687 | 1,354 | +53 |
| 793 | 4,607 | 1,355 | −80 (GC) |
| — | 4,995 | 1,355 | +388 (one area load) |
| — | 5,104 | 1,355 | FATAL |

Headroom passes: 4948 → 4936 → 4958 (64 MiB each), then 4977 → 5028 → 5045 → 5077 → 5110 (0 MiB
each). Same failure as `.504`, same 16 MiB `HostUncached` allocation:

```
[uwp-vk] FATAL vmaCreateBuffer(16384 KiB, type=HostUncached) -> -2; Xbox AppMemory 5104/5120 MiB
    (guest pinned=1355 MiB); VMA heap[0] block=2140 MiB alloc=2134 MiB usage=2140 MiB budget=4096 MiB
GPU [Debug] <Critical> buffer.cpp:189 Create: Assertion Failed!
```

## What it proves

`guest pinned` climbed 0 → 1,355 MiB while `pageable private` (the pager's only source) stayed near
400 MiB, so five emergency page-out passes in a row returned nothing. The pin rule pinned every page
a guest thread first touched, not just the shared/WC_GARLIC windows — §2j has the cause, the fix and
the `BB_UWP_PIN_PRIVATE=1` A/B switch.
