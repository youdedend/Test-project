# Console run 9 — `.823` (2026-10-27): the budget raise worked, the ceiling is next

Logs: `loader.log.txt` (1,032 lines), `errors.log.txt` (77). Build identity: the pager line ends
`disk budget=768 MiB (ceiling 1024 MiB)` — the first run with the growing budget.

## The raise did exactly what it was for

```
UWP swap: disk budget raised to 1024 MiB (ceiling 1024 MiB) for a waiting allocation
UWP swap: paged out 62 MiB on request (0 MiB of it urgent)
UWP buffer headroom: swap paged out 62 MiB for a 65536 KiB allocation in 220 ms (usage 4958/5120 MiB)
```

One step, taken during the area load, and the pager filled it. No `FATAL`, no assertion, no `STOP` —
third run in a row that reached the wall without stopping.

## Where it ended: the 1 GiB ceiling, with the device still willing

```
UWP swap: request 43 MiB got nothing; scanned=1257248 ... at-budget=234572 ...;
    disk=1024/1024 MiB full=0 write failures=0;
    page table: resident=916 pinned=0 section=0 shared=0 unreadable=0 MiB
UWP swap: disk pages=1024 MiB of a 1024 MiB budget (ceiling 1024 MiB, raised 1); write failures=0
UWP STALL DETECTED (presents stuck at 832 for 10 s)
UWP lifecycle: suspending; memory=5111/5120 MiB
```

`at-budget=234572` pages = 916 MiB refused by the ceiling **alone**, with `full=0` and
`write failures=0`: the device never refused a byte. Total paged out in the run: 1,033 MiB.

The stall is the same shape as `.736`: it appeared at 4,972 MiB (before the ceiling was reached) and
`presents` never moved again (832 for the rest of the run) — the log has no way to say whether the
pager's exclusive lock was involved, which is what `.9xx` adds.

## Fixes in the next build

| What | Where |
|---|---|
| Ceiling 1 GiB → **1.5 GiB** (start 768, step 256), so `.823`'s own step repeats twice. `.559` had writes refused at 1,252 MiB, so the second step should fit and the third finds the edge — and a refusal is fail-soft (`full=1`, growth stops, page stays resident) | `guest_swap.inc:24-46, 878-889` |
| Growth stops if the sweep saw a write failure for any other reason (no point raising a limit writes are not reaching) | `guest_swap.inc:545-566` |
| **Images are reclaimed before an allocation fails**, when the pager under-delivers (`paged < want`): the ladder's second rung on the GPU thread, bounded to 128 MiB, at most once a second, same structural rules as the emergency path | `buffer.cpp:180-225` |
| `runtime_memory_pager_state()` — printed into the stall dump: whether a sweep holds the commit lock and for how long, the last page-out request and when it finished, out/in/disk/budget | `guest_swap.inc:677-701`, `main.cpp:58, 1895` |

## Memory in this run

`4456 → 4776 (+320) → 4972 (+196)` MiB in three samples while `UWP dense GPU arena` climbed to
`live 928 MiB, 58/176 DMA views`. `Runtime: direct memory ... live=4812046336, budget=5301600256` —
the guest's own direct memory is 4.48 GiB of the 5 GiB app budget and the pager cannot touch it;
what is pageable is the ~1.2 GiB below it, and that is what the swap budget bounds.
