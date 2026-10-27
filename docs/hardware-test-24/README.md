# Console run 10 — `.908` (2026-10-27): the device itself refuses at 1,171 MiB

Logs: `loader.log.txt` (1,117 lines), `errors.log.txt` (83). Build identity: pager line ends
`disk budget=768 MiB (ceiling 1536 MiB)`.

## The growth worked, twice — then the device said no

```
UWP swap: disk budget raised to 1024 MiB (ceiling 1536 MiB) for a waiting allocation
UWP swap: disk budget raised to 1280 MiB (ceiling 1536 MiB) for a waiting allocation
UWP swap: paged out 90 MiB on request … for a 98304 KiB allocation in 295 ms (usage 4928/5120 MiB)
UWP swap: disk pages=1171 MiB of a 1280 MiB budget (ceiling 1536 MiB, raised 2);
    write failures=1; urgent sweeps=0 (0 MiB) (device full: private pages stay resident)
```

`raised 2` and 1,171 MiB stored. Then **one** failed write: `write failures=1`, `full=1`. That is
the real edge — `.559` was refused at 1,252 MiB, this run at 1,171. The room on that console is
about 1.2 GiB, whatever number the port asks for.

## What the one refusal cost, and it was everything

After it, the pager was dead for the rest of the run:

```
UWP swap: request 64 MiB got nothing; scanned=0 unmapped=0 unpageable=0 … at-budget=0 …
    disk=1171/1280 MiB full=1 write failures=1;
    page table: resident=767 pinned=0 section=0 shared=0 unreadable=0 MiB
```

`scanned=0` — the sweep broke out before examining a single page, because
`if (uwp_swap_disk_full) break;` sat at the top of the loop. 767 MiB of guest memory was still
pageable and the app was 780 MiB from its limit; it climbed to 5,111/5,120, the frame rate fell to
5.2 FPS, and the session ended with `Fault: exception 0xc00000fd` (host stack overflow, thread 3728,
in the present path) — a failure mode this port had not produced before.

## What this decides

The room is fixed. The pages have to get smaller. Both available levers are in the next build:

| What | Where |
|---|---|
| **Pages are stored compressed** (LZ4 block per page, `xbox/platform/swap_codec.h`), in a 4 KiB-aligned packed arena with one 32-bit record per physical page; all-zero pages are not written at all | `guest_swap.inc:70-100, 240-430` |
| A rewarded write re-uses its record when the new payload fits (a view saved twice does not grow the file) | `guest_swap.inc:385-400` |
| The budget and every report now count **bytes the file actually occupies**, not `pages × 16 KiB` | `guest_swap.inc:974-1000` |
| `full` is no longer permanent: a refused write is retried after 10 s | `guest_swap.inc:52-59, 655-670` |
| A full device **no longer stops the walk**: view pages (`kind == 3`) free RAM without needing disk, so `scanned=0` cannot happen again | `guest_swap.inc:700-712` |
| A record that would exceed our own budget reports `ERROR_DISK_QUOTA_EXCEEDED`, never `ERROR_DISK_FULL` — a full budget must not look like a full device | `guest_swap.inc:395-408` |
| The store's page buffers take their own innermost lock (`uwp_promote` reaches a transfer without `uwp_commit_lock`) | `guest_swap.inc:336-342, 420-428` |
| `xbox/tests/swap_codec_test.cpp` round-trips zero, random, heap-like, text-like and boundary-length pages and rejects malformed input; added to CI | `.github/workflows/xbox.yml`, `xbox/tests/swap_codec_test.cpp` |

Measured on the host (16 KiB pages, `-O2`): heap-like data 3.4 KiB/page (21%), zeros 0.15 KiB, at
~10 µs per page. A page-out of 16 MiB is ~1,000 pages, so ~10 ms of coding on top of the ~7 ms of
I/O the sweep already spends.

Expected: the same 1.17 GiB of device room holds roughly 2–4× the guest memory, i.e. the ~780 MiB
that was missing at the crash with room to spare. **Unproven until the console prints the ratio**
(`… pages stored (N raw, N blank, N MiB of payload, N% of raw)`).

## Open

- `Fault: exception 0xc00000fd` on the host stack in the present path, at the memory limit — not
  investigated. If it recurs in the next build, the thread and RVA are in `errors.log.txt`.
