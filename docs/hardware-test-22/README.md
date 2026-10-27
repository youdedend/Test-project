# Console run 8 — `.736` (2026-10-27): the crash is gone, the wall is the disk budget

Logs: `loader.log.txt` (1,506 lines), `errors.log.txt` (77). Build identity: the pager line reads
`disk budget=768 MiB` with no `ceiling` field and the report carries `urgent sweeps=0 (0 MiB)`, so this
is `.736` (`3dec03a`) — the first run with the emergency reclaim that ignores the age gate.

## The run did not stop

No `FATAL`, no assertion, no `STOP` this time. The previous two runs both died on
`vmaCreateBuffer(…) -> -2`; this one reached the wall and **kept going**:

```
Texture cache: emergency reclaim freed 117 MiB of the 116 MiB asked for;
    seen=610 too recent=0 gpu-written=149 bound/target=1 no backing=3 (gc_tick=1251)
UWP memory emergency (allocation failed): need 116 MiB; paged out 0 MiB in 7 ms,
    freed 117 MiB of images in 214 ms (usage now 5025/5120 MiB)
```

The age-gate fix worked: `seen=610` (was `seen=0`) and it freed more than it was asked for.

## What it then hit: the swap budget, not the device

```
UWP swap: disk pages=768 MiB of a 768 MiB budget; write failures=0; urgent sweeps=0 (0 MiB)
UWP swap: pageable private=1209 pinned=0 MiB; page-out passes=689 with nothing to give=275
UWP swap: not paged - pinned policy=0 section=0 shared=0 exec/unreadable=0 MiB
UWP swap: request 64 MiB got nothing; scanned=1257248 unmapped=0 unpageable=444960
    kind=196724 uncommitted=306024 views-off=0 pinned=0 shared=0 section=0 at-budget=0
    unreadable=0 view-guard=0 recent=0 write-failed=0; disk=768/768 MiB full=0
    write failures=0; page table: resident=1209 pinned=0 section=0 shared=0 unreadable=0 MiB
```

Read together, the three lines are unambiguous:

- the sweep took **nothing** for a reason the old report could not name: the pages were skipped by
  `uwp_swap_disk_pages * PAGE >= uwp_swap_disk_budget`, i.e. the self-imposed 768 MiB ceiling;
- `at-budget=0` in that line is a **missing counter** in `.736` (the increment was not inserted when
  the sweep was split into buckets) — it is the only bucket the 309,540 unaccounted pages fit;
- everything else is zero: 1,209 MiB of pages passed every structural test and could not be written
  because the budget was full. `full=0` means **the device never refused a write** — 768 MiB is a
  number this port chose, not a limit the console imposed.

End of life: `UWP STALL DETECTED (presents stuck at 1291 for 10 s)`, then
`UWP lifecycle: suspending; memory=5116/5120 MiB` (the user suspended it). Usage sat at 5,116/5,120 MiB
with 1,209 MiB of pages it was not allowed to write out.

## Fixes in the next build

| What | Where |
|---|---|
| The disk budget grows on demand: only while an allocation is waiting (`runtime_memory_page_out`), 256 MiB a step, 768 MiB → **1 GiB ceiling**, `BB_UWP_SWAP_DISK_MB` still a hard cap. Past the ceiling, or on `ERROR_DISK_FULL`, the page stays resident exactly as before | `guest_swap.inc:24-46, 537-558, 831-841` |
| `at-budget` counter added (the missing bucket) | `guest_swap.inc:545` |
| No repeated full-table scan while the disk is the blocker: if the previous request was refused by the disk and less than 500 ms has passed, answer at once instead of walking 2× the page table under the commit lock | `guest_swap.inc:666-675, 706-712` |
| Report and pager line print `ceiling` and `raised` | `guest_swap.inc:786-792, 859-864` |

The 10 s stall is what an allocation storm through that lock looks like: `paged out 0 MiB on request`
fourteen times, 7–8 ms each, all under `uwp_commit_lock`, then `presents stuck`.

## Still open

- `gpu-written=149` images were refused by the emergency reclaim. They hold GPU output and cannot be
  written back from a tiled layout (upstream PR #89 documents the same thing). Not touched yet.
- Whether 1 GiB of swap is enough for this area load: unknown until the next run.
