# Hardware test 29 — what build `.819` measured, and what it changed

Run analysed: `loader.log.txt` / `errors.log.txt` from **1.0.26301.819**
(`UWP build: version=1.0.26301.819 commit=6c560c4557c1 branch=arena/8ac58bdb-test-project`).

Result of the run: **the app did not die of an allocation failure this time.** It ended with
`Wait presentation bridge: Vulkan -4` — `VK_ERROR_DEVICE_LOST` — after ten seconds of stalled
presentation, at **4989/5120 MiB**: under the limit, not at it.

## 1. The measurements the table was built for

Peak of the last healthy sample (line 2103 of the log):

| Category | Value |
|---|---|
| app | 5055 / 5120 MiB |
| guest resident | **2688 MiB** (pinned 0) |
| textures | 301 MiB in 160 images (673 images had just been evicted) |
| dense arena | **live 912 MiB, retired 448 MiB** |
| swap file | **483 MiB of a 483 MiB budget** (2,048 asked, capped to free − 384 reserve) |
| swap contents | 73,508 pages = **1,149 MiB of guest memory** in 483 MiB of file |
| swap write failures | **6,828** (all refused by the 384 MiB reserve: `device free=383 MiB`) |
| fences | 1666 submitted / 1664 completed — GPU idle while the app stalled |
| pageable private | 806 MiB of the 2688 MiB resident |

## 2. What this run proves

**A3 pays for itself, measured on the console.** 73,508 stored pages are 1,149 MiB of guest
memory. The file that holds them is 483 MiB. Blank-page elision accounts for 19,808 pages = 309 MiB
at zero bytes; compression accounts for the rest. Without A3 the same protection would need
**1,149 MiB** of disk, i.e. 2.4× the budget — and this console only has 867 MiB free. So A3 is not
an optimisation here, it is the reason the pager works at all on a full device.

**A2's cap is correct and this console is the case it was written for.** `disk budget 2048 MiB
capped to 483 MiB (device free=867 MiB, reserve 384 MiB)`. Raising the default to 2048 MiB changed
nothing on this device — it has 867 MiB free. The budget is no longer the limit on this console; the
**device's free space** is.

**The disk is now the wall.** `disk full=1`, 6,828 refused writes, and the pager kept sweeping.
Every allocation that arrived after that point scanned the whole page table, skipped every private
page as `at-budget`, released nothing, and returned zero: `page-out passes=722 with nothing to give=251`.

**That thrash is the crash.** `UWP STALL DETECTED (presents stuck at 877 for 10 s)`, a PM4
submission taking 2,320 ms (`UWP thread op: tid 7020 in 'processing a PM4 submission' … for 2320 ms`),
then the presentation fence went past its 10 s wait and the device was lost. The fences show the GPU
was idle: this was the CPU side of the pager, not a GPU hang.

## 3. Two of my own diagnostics were wrong, and are fixed

- `at-budget=206460 (164162 MiB)` — 206,460 pages are 3,226 MiB, not 164,162. The page counter was
  reset per request and the byte counter was not. Fixed in `uwp_swap_scan_reset`
  (`xbox/platform/guest_swap.inc:185`).
- `accounted=8163 MiB, unaccounted=-3108 MiB` against an app of 5,055 — the sum added the swap file
  (disk, not RAM) and the GPU's device total (which on a console already contains the guest pages).
  Rewritten to `guest resident + (device − resident)` (`xbox/platform/main.cpp:1819`), which cannot
  exceed the app total by construction.

## 4. What changed in response (build after `.819`)

| Change | Where | Why |
|---|---|---|
| A full swap file no longer costs a page-table walk | `guest_swap.inc:884` (`uwp_swap_disk_blocked`), used at `:889` | The 10 s stall and the lost device. A page that cannot be written must not be walked to; views are still swept because they free RAM without disk |
| One line a second while the swap is blocked | `guest_swap.inc:895` | The log now says "the swap is full and the sweeps are no-ops" instead of leaving it to be inferred |
| Emergency reclaim releases retired dense arenas first | `gpu/shim/bbgpu.cpp:471` | 448 MiB was sitting retired while the app was at its ceiling. Only arenas the fence has passed and no DMA view holds are freed |
| Resident memory split by what the pager may do | `guest_swap.inc:1289` (`runtime_memory_resident_breakdown`), printed at `main.cpp:1763` | 2688 MiB resident with only 806 MiB pageable: the next log says whether the other 1.9 GiB is private-but-unpageable, section-backed or a view |
| Device loss is reported with full context | `present_bridge.cpp:181`, `main.cpp:1486` | `Vulkan -4` alone said nothing; now a `UWP DEVICE LOST` line prints app, resident, swap, textures, arena and fences, then dumps the breadcrumbs |
| A breadcrumb every heartbeat, and on level changes | `main.cpp:1836`, `bbgpu.cpp:388` | The ring held **one** entry at the moment of the crash. It now holds ~16 minutes of the climb |
| The swap line prints what A3 saved | `main.cpp:1760` | "keeps out of RAM: N MiB of guest pages in M MiB of file" |

## 5. The number to watch next run

The app reached its ceiling with **2,688 MiB of guest pages resident and only 806 MiB of them
pageable**. The pager cannot help with the other 1,882 MiB, and the new `resident:` line is what says
whether those are WB_ONION heaps, audio buffers, command rings, section-backed pages or views. That
is the 1.9 GiB to go after — not the texture cache, which was already down to 160 images and 301 MiB.

Series S/X reproduction is still required. This build changes behaviour (no more full-disk sweeps,
arena release under pressure) and adds measurement; neither is claimed fixed.
