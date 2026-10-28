# Hardware test 28 — the memory table, the breadcrumbs and the big-allocation lines

Build: **1.0.26301.819**, tag `xbox-alpha-1.0.26301.0819`, commit `6c560c4` (code `c9f85f1`).
CI run: <https://github.com/youdedend/Test-project/actions/runs/40654986178> — green.
Asset: `BloodborneXbox-1.0.26301.819.zip`, 10,217,597 B, sha256
`f5baf919a39d9504b0b0f62a8bb92037069ff8a4b37f56b6f5648027233eba87`.

Nothing here is claimed fixed. This build changes what the log *records*; it does not yet change how
much memory the game takes, except for the A2 budget and the A5 resolution default, both of which
are deliberately visible in the log and reversible from `bbport.ini`. The first console run with
this build is the measurement.

## The eleven items, and where each one lives

| # | Item | Where | State in this build |
|---|------|-------|---------------------|
| A1 | count the at-budget skip in bytes | `xbox/platform/guest_swap.inc:978`, `:1018` (both skip sites), printed in the "got nothing" line | done — `at-budget=136690 (534 MiB)` |
| A2 | swap budget 2048 MiB default, capped to the device, ini key | `guest_swap.inc:1385-1420` (budget + cap), `main.cpp:1955-1974` (`swap_disk_mb=`) | done — the cap line prints the real number |
| A3 | blank skip + compression behind flags, with counts | `guest_swap.inc:424-445` (flags), `:520`, `:529` (use), report `:1330` | done — `BB_UWP_SWAP_BLANK`, `BB_UWP_SWAP_COMPRESS` |
| A4 | per-category memory table at each heartbeat | `main.cpp:1673-1790` (`log_memory_table`), called `:2296` | done |
| A5 | 720p, upscaler and frame-gen off by default, cost printed | `gpu/shim/bbport_settings.h:76-84` (`OutputDefault`), `main.cpp:686` (migration), `:1589` (cost line) | done — one-time logged migration |
| B6 | 5 s heartbeat, own thread, flushed, GPU fence + paging | already on its own thread; the new fields `main.cpp:1673-1790` | done |
| B7 | per-thread operation tags with elapsed time | `gpu/shim/bbgpu.cpp:552-612`, tags in `vk_scheduler.cpp:326`, `:375`, `vk_draw_prep.cpp:506`, `liverpool.cpp:161`, `guest_swap.inc:1160` | done — printed in the stall report (`main.cpp:2327`) |
| B8 | allocations ≥32 MiB and anything over 250 ms | `gpu/shim/bbgpu.cpp:516`, hooks at `buffer.cpp:263`, `image.cpp:252`, fence wait `vk_scheduler.cpp:386` | done |
| B9 | fail soft, hard timeout, never block forever | `guest_swap.inc:159-176` (`BB_UWP_SWAP_TIMEOUT_MS`), check in the sweep `:899`, `runtime_memory_page_out` `:1159` | done — the sweep stops with a line |
| B10 | breadcrumbs for suspend/resume/limit/termination, 200-line ring | `main.cpp:1440-1480` (ring), hooks `:137`, `:157`, `:2117`, `:2203`, dumps `:1488` (exception), `:1849` (std::terminate) | done |
| B11 | log header + per-run timestamped log | `main.cpp:1589-1660` (header), `:1770-1832` (per-run archive) | done |

## Measured MiB saved — what is a measurement and what is not

Only the pager's own savings are measured, and they were measured on the previous build's console
log (`.524`). The rest is what this build now prints so the next run can measure it. Numbers marked
*estimate* are arithmetic on the target sizes, not console readings.

| Item | MiB | Source |
|------|-----|--------|
| A3 blank-page elision | **309 MiB** not written to disk (19,770 pages × 16 KiB; stored as 2-byte records) | `.524`: `19,770 blank` of 92,196 pages |
| A3 page compression | **≈594 MiB** saved on the non-blank pages (72,426 × 16 KiB = 1,131 MiB compressed into 537 MiB of payload) | `.524`: `537 MiB of payload, 37% of raw` |
| A2 swap budget raise | **0 MiB proven, up to +1,280 MiB of capacity** *estimate* — `.524` stopped at 713 MiB of a 768 MiB budget with `device free=349 MiB`, so the free-space cap, not the budget, was binding. The cap is now printed, so the next run says how much the device really grants. | `.524` swap report |
| A5 720p instead of 1080p | **≈4.8 MiB per full-size RGBA8 host target** *estimate* (8.29 MiB at 1080p → 3.52 MiB at 720p); the header prints the target and history-pair sizes, and the memory table prints `gpu: device=` / `textures=`, so the run measures it instead of us estimating it | `main.cpp:1616` feature-cost line |
| A1 at-budget bytes | 0 MiB — a counter. `.524` skipped 136,690 pages at the budget, i.e. **534 MiB of guest memory that could not be paged out**; that number is the size of the gap that has to be closed elsewhere | `.524`: `at-budget=136690` |
| B6–B11 | 0 MiB — diagnostics; they cost one page walk per heartbeat (≈1.25 M entries, a few ms) and one clock read per fence wait | — |

What is *not* in this table, and must not be guessed: the 576 MiB of headroom that separates our
5,116 MiB world load from the 4,540 MiB reference. Items A1–A5 exist to find it, and the next run's
`unaccounted=` line is the first number that can say whether it is ours.

## What the next log will show that no log has shown before

1. **The header**, at the top of `loader.log`:
   ```
   Bloodborne Xbox Series X|S UWP loader started 2026-10-28 12_34_56
   UWP build: version=1.0.26301.819 commit=<sha> branch=arena/...
   UWP memory marks: limit=5120 MiB, usage=... MiB, level=., pinned=... MiB, resident=... MiB
   UWP settings: output_res=1280x720 upscaler=off frame_gen=off preset=... swap_disk_mb=...
   UWP feature cost: output 1280x720 (scene target 3.5 MiB, history pair 7.0 MiB); ...
   UWP flags: swap=1 verify=1 blank=1 compress=1 pin_private=0 timeout_ms=2000 toggles=...
   ```
2. **The memory table**, every 5 s (every 1 s near the limit):
   ```
   UWP memory table: app=5116/5120 MiB guest resident=... MiB (pinned ... MiB)
     swap: used=... budget=... payload=... MiB, pages=... (blank ...), blank=... raw=... compressed=... MiB
     swap io: out=... in=... pages, copy-in failures=..., write failures=..., at-budget=... MiB, ...
     gpu: device=... MiB (available=1), textures=... MiB in ... images
     arena: live=... retired=... reserve-used=... MiB
     fences: submitted=... completed=... outstanding=...
     os: hard faults=..., working set=... MiB, peak=... MiB, page file=... MiB, presents=...
     accounted=... MiB, unaccounted=+-... MiB (app - guest resident - swap - device)
   ```
   `unaccounted` is the line to read first: large and positive means the bytes are neither guest
   memory nor the GPU's, so no reclaim of ours can return them.
3. **The big and slow allocations**, when they happen:
   ```
   UWP alloc: 384 MiB image (texture cache), usage 4210 -> 4594 MiB, 41.2 ms
   UWP alloc (small but slow): 12 MiB buffer (buffer cache), usage ... , 512.0 ms [SLOW >250 ms]
   UWP fence wait: 1832.4 ms for tick 918273 (GPU at 918270) [SLOW >250 ms]
   ```
4. **The sweep timeout**, when the pager runs out of time rather than pages:
   ```
   UWP swap: sweep stopped at its 2000 ms timeout (released 128 MiB of the 256 MiB asked; budget=...)
   ```
5. **The budget cap**, once at start-up:
   ```
   UWP swap: disk budget 2048 MiB capped to 1613 MiB (device free=1997 MiB, reserve 384 MiB)
   ```
6. **Per-thread operations in the stall report**:
   ```
   UWP thread op: tid 1234 in 'waiting on a GPU fence' (0x1f4, 0x1f0) for 9123 ms
   UWP thread op: tid 5678 in 'preparing draws (DrawRec)' (0x1f6, 0) for 1204 ms
   ```
7. **Breadcrumbs before a crash**, so a fault says what the app was doing:
   ```
   UWP breadcrumbs (unhandled exception), last 200 of 200 entries:
     [412300 ms] memory limit changed 5120 -> 4608 MiB
     [418002 ms] suspending: memory=4890/5120 MiB, presents=18412
   ```
8. **Per-run logs**: `loader.log` still holds the current run (that is what gets sent in), and every
   previous run is kept as `loader-<start time>.log` instead of one rolling `.prev` file.

## What to send after the run

The same two files as before — `loader.log.txt` and `errors.log.txt`, taken straight after the
crash and before relaunching. The per-run archives are additive: they exist so two runs can be
compared, not instead of the two files.

## Hardware requirements

Series S/X reproduction is required before anything here is called fixed. The A5 default change
(720p) and the A2 budget are the only items that can change the outcome by themselves; everything
else is observation.
