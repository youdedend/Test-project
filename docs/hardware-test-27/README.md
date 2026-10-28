# Hardware test #27 — `.524`: the swap held, then a fault handler wanted 64 KiB of stack

Logs: `loader.log.txt` (1,196 lines), `errors.log.txt` (85), `dozen.log.txt`. Sent after the session
ended. Run date 2026-10-28.

Build: `1.0.26301.524` (tag `xbox-alpha-1.0.26301.0524`, commit `d4f18dd`). Identified by the
`codec fallbacks=` field in the `disk pages=` line and by `reserve 384 MiB` in the `device free=`
line, both new in that build.

## What `.524` fixed, measured

| Reading | This run (`.524`) | `.421`/`.437` (`docs/hardware-test-26`) |
|---|---|---|
| Device reserve in force | **yes** — `device free=1008 MiB (reserve 384 MiB …)` from the first report on | no — printed `reserve 0 MiB` |
| Records refused by the reserve | **1**, at `device free=349 MiB` | 0 (the check was never armed) |
| Codec fallbacks | **0** over 92,196 stored pages | field did not exist; the run stopped on a codec collision |
| `corrupt=0` | **yes** — 954 verified pages, `corrupt=0` at the end | stopped on the 500th verify |
| Pages stored | 92,196 (715 raw, 19,770 blank, 537 MiB payload, **37% of raw**) | 65,475 (27% of raw) |
| Guest memory living in the swap | **1,439 MiB** in a 713 MiB file, `disk=1439/768 MiB` | — |
| `write failures` | 1 (the reserve refusal), then 597 refusals as the device stayed below the reserve | 0 |
| Frame rate | 28.8 → 29.4 FPS while it was healthy, 5.7 FPS at the end | 6.2 FPS |
| Ended with | `Fault: exception 0xc00000fd` (stack overflow) | `STOP: UWP promoted page integrity` |

Three things to take from that table:

1. **The 16 KiB codec collision is gone.** 92,196 pages stored, `codec fallbacks=0`, `corrupt=0`, and
   no integrity stop. The run went 40% further into the session than `.421`/`.437` did (presents
   826 vs 844 with far more stored pages, and the swap was still taking pages at the end).
2. **The device reserve did its job.** At `device free=349 MiB` the store refused a record instead of
   taking the game's last 349 MiB. The session continued after that refusal; the `.1119` failure
   (`WriteSpan` assertion on a full device) did not recur.
3. **The pager still is not the stall.** `UWP STALL DETECTED (presents stuck at 826 for 10 s)` with
   `lock=sweep held=0 ms sweeps=386 request=55 MiB finished 16 ms ago` — the fourth run in a row.

## The stop: a stack overflow inside our own fault handler

```
Fault: exception 0xc00000fd at bloodborne.exe+0xc52457 (thread 5104)
  [rsp+0x10] bloodborne.exe+0x8c96c
  [rsp+0x28] bloodborne.exe+0x9128c
  [rsp+0xd8] bloodborne.exe+0x8d54d
  [rsp+0xf8] bloodborne.exe+0x8d167
```

`0xc00000fd` is STATUS_STACK_OVERFLOW. The addresses mean nothing without the package's own linker
map, so they were added to `xbox/scripts/crash-rvas.txt` and CI resolved them against `bloodborne.map`
for a build of the identical source (run `40644126681`, whose only change was the RVA list):

| RVA | Symbol |
|---|---|
| `0xc52457` | `_alloca_probe` (+0x37) — where the probe faulted |
| `0x8c96c` | `uwp_swap_open` (+0xc) |
| `0x8d54d` | `uwp_commit` (+0xbd) |
| `0x9128c` | `uwp_swap_page_out_locked` (+0x3c) |
| `0x8d167` | `uwp_page` (+0x17) |

That is a complete call chain, and it is ours end to end:

```
guest thread takes a commit fault
  → runtime_memory_handle_commit_fault_at → uwp_page
    → uwp_commit (no memory left for the page)
      → uwp_swap_page_out_locked (free something else first)
        → uwp_swap_open  ← declared wchar_t wide[32768]
          → _alloca_probe ← walked 64 KiB down the guest thread's stack
```

`wchar_t wide[32768]` is **64 KiB of stack frame**, and MSVC allocates it in the prologue — on
*every* call, including the early-out that finds the swap file already open. The chain above runs on
the faulting **guest thread's** own stack (a UWP guest thread runs on a low stack of
`requested + 256 KiB`; the threads in this log requested 64 KiB, so 320 KiB total). A guest ~256 KiB
deep plus a 64 KiB frame from the thing that is supposed to *recover* from the fault is a stack
overflow, and there is no recovering from one.

It also explains the older mystery: the same `0xc00000fd` ended the `.908` run
(`docs/hardware-test-24`) at `bloodborne.exe+0xc50ef7` and was written off there as "the host stack in
the present path". It was not the present path. Same bug, different build.

## The fix (`.26301.655`)

| Piece | Where | What it does |
|---|---|---|
| Path conversion on the heap | `guest_swap.inc:303-317` `uwp_swap_wide_path` | UTF-8 → wide with `MultiByteToWideChar` twice, buffer from `calloc`, freed by the caller |
| The open path split | `guest_swap.inc:318-345` | `uwp_swap_open` is four lines and allocates nothing when the file is already open; `uwp_swap_open_slow` does the work |
| The free-space query too | `guest_swap.inc:118-132` | it also runs inside a page-out, on whatever stack that page-out used, so its `wchar_t wide[1024]` went the same way |
| A stack overflow names its thread | `src/probe.c:312-330`, `runtime_thread_current_name` in `src/runtime_thread.c:216-223` | prints RSP, TIB `StackBase`/`StackLimit`, the remaining headroom in bytes, and the guest thread name — the next one needs no symbolication round |
| A build-time guard | `.github/workflows/xbox.yml`, "Guard - no large stack frames in the pager" | a local buffer of 4 KiB or more in `guest_swap.inc`/`guest_memory.inc` fails the build; `static` buffers are exempt |

`STACK_MARGIN` was deliberately **not** changed. With the 64 KiB frame gone, the guest had ~64 KiB
of its stack left at the moment it died and the handler needs under 8 KiB, so the immediate cause is
removed. Raising the margin would have cost ~1 MiB per guest thread against the same 5 GiB budget
with nothing measured to justify it. The new line prints the headroom, so if an overflow happens
again the log will say how deep the guest actually was instead of leaving it to arithmetic.

## Still open

- **The working set.** `pageable private=508 MiB` and 4 dry passes at the end, `request 64 MiB got
  nothing` with `at-budget=136690`, `disk=1439/768 MiB full=1`. The device reserve now protects the
  game's files, but that also means the swap stops at 713 MiB of file with ~350 MiB of device still
  reserved and unusable — the two are the same wall from both sides.
- The session was at 1280x720 from the start again (`CoreWindow presentation bridge: 1280x720`), so
  `.437`'s resolution rung had nothing to lower and never printed.

## What to send next time

`loader.log.txt` and `errors.log.txt`, right after the session ends, before relaunching.

- `grep "stack overflow" errors.log.txt` — should be absent; if present it now names the guest thread
  and the headroom, which decides whether the margin is the next lever.
- `grep "codec fallbacks\|device free=" loader.log.txt` — the codec counter and the reserve.
- `grep "UWP swap: disk pages" loader.log.txt` — stored pages and the compression ratio.
