# Console run 7 — `.717` (2026-10-27, scene load)

Logs: `loader.log.txt` (857 lines), `errors.log.txt` (79 lines). Build identity: the pager line reads
`app budget=5120 MiB, pressure headroom=640 MiB; private pages pageable; disk budget=768 MiB` and the
report ends `urgent sweeps=…` — so this is `.717` (`3fe9da1`), the first run with the urgent sweep.

## What the ladder did

| Step | Result |
|---|---|
| Disk backing | `disk pages=367 MiB of a 768 MiB budget; write failures=0` — **no disk-full**, half the budget still free |
| Stalls | `grep -c STALL` = **0**; slowest frame 575.5 ms once at start-up, then ≤ 52.4 ms |
| Pinned guest memory | `pinned=0 MiB` in every report and every telemetry line |
| Page-out on request | 9, 9, 14, 6, 9 MiB for 16 MiB asks (25–105 ms), then **ten requests in a row that took 0 MiB in 7 ms** |
| Urgent sweeps | `urgent sweeps=0 (0 MiB)` in every report — the second-chance bit was never what stopped it here |
| Image reclaim | `emergency reclaim freed 0 MiB of the 132 MiB asked for; seen=0 …` |

Then the same wall as `.649`:

```
UWP memory emergency (allocation failed): need 131 MiB; paged out 0 MiB in 7 ms,
    freed 0 MiB of images in 0 ms (usage now 5060/5120 MiB)
[uwp-vk] FATAL vmaCreateBuffer(98304 KiB, type=DeviceLocal) -> -2; Xbox AppMemory 5060/5120 MiB
    (guest pinned=0 MiB); VMA heap[0] block=2313 MiB alloc=2307 MiB usage=2313 MiB budget=4096 MiB
buffer.cpp:235 Create: Assertion Failed! / Failed allocating buffer with error ErrorOutOfDeviceMemory
```

## What this run proved, and what it left open

- **The disk half is not the problem any more** (367 of 768 MiB, zero write failures, zero stalls).
- **The second-chance bit is not the problem either**: the urgent sweep never had to fire, so the
  pages it was written for were not what the sweep was rejecting.
- **`seen=0` in the texture line is a bug in the reclaim call, not a full cache.** `ForEachItemBelow`
  stops at the first item newer than the gate, and `EmergencyReclaim` asked for `unused_seconds = 0`,
  i.e. the tick recorded at the start of this second. During an area load every image is created or
  touched after that tick, so the walk never entered its loop. Fixed in `.733` (no age gate for the
  emergency path).
- **The guest pager still reported 1,534 MiB of `pageable private`** with `section=0 shared=0
  pinned=0 exec/unreadable=0` while ten consecutive requests took nothing. The report and the sweep
  used the same rules, so one of them is looking at stale state; the next build prints the sweep's own
  per-reason counters and re-counts the page-table buckets at the moment the request fails.

## Memory growth in this run

`4367 … 4402 (presents 791) → 4572 → 5020 MiB (presents 792, level=2, +447 MiB in one sample)`,
while `UWP dense GPU arena` climbed 16–64 MiB per chunk to `live 960 MiB, 60/176 DMA views`. The
jump happens across two presents — an area load, not a leak per frame.
