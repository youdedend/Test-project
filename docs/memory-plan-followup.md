# Xbox memory follow-up: builds 1.0.26301.1127, .1156 and .813

This is an engineering build, **not a hardware-verified fix**. The baseline is the user's summary:
all three runs reached the 5,120 MiB app limit; presents stopped at 793, 1,130 and 867 respectively.
The .813 run had 88 MiB free on Q: at launch, 4 MiB at the end, 39,589 refused swap records and
1,157 MiB of pageable private memory still resident. D: reported 76.7 GiB free and writable, while
E: was the USB dump volume. It also reported 848 MiB live / 128 MiB retired arenas, 501 MiB of
textures, 49 candidates blocked as tiled GPU-written, and 822 pool decommit failures. Its final
reported burst was 4,507 -> 5,033 MiB in under two seconds. The .813 pool's 287 MiB saved was the
*old counter's report*, not an independently verified reduction: the old counter failed to subtract
page-ins and failed to account accurately for pages that remained committed.

## Configuration

Each feature has its own `bbport.ini` key; a missing key uses the shown default. The run header
prints the effective switches. Existing files are not overwritten just to add the new keys. Add
these lines if an A/B run needs to change them:

```ini
swap_auto=1
swap_file=auto
swap_speed_test=1
swap_pool_decommit=1
arena_admission=1
arena_live_cap_mb=768
watchdog=1
watchdog_fast=1
watchdog_fast_tick_mb=600
watchdog_reduce=1
watchdog_reduce_mb=96
texture_reclaim_experiment=0
resolution_diagnostics=1
cache_run_diagnostics=1
```

`BB_UWP_SWAP_FILE` takes precedence over `swap_file`, and `swap_disk_mb` (default 2048) and
`swap_reserve_mb` (default 384) are still honoured. Explicit paths are also checked for a known
non-USB volume (LocalState itself is the fallback), accessibility and existing files. No existing
file is truncated: backing and benchmark files use unique names/CREATE_NEW and delete-on-close.

**Important:** `arena_admission=1` enables the requested *measurements only*. The 250 MiB
headroom and 768 MiB live-arena cap are not enforced. There is no safe smaller-merge or skip path
in this build. The log explicitly says `OBSERVE ONLY`. See item 2 below.

## Items and evidence

| Item | Implemented | Console evidence to collect; not a claim of savings |
|---|---|---|
| 1. Swap location | Probe writable fixed volumes and their storage bus; select the eligible non-USB one with most free space (D: wins a tie), otherwise fall back to LocalState. Strict `swap_file` / `BB_UWP_SWAP_FILE` validation, 2,048 MiB default budget, reserve accounting, a 64 MiB unbuffered write/read test when enough free space remains, and a LocalState fallback if opening the chosen sparse file fails. | `UWP swap selection`, `UWP storage ... type=... bus=...`, `UWP swap speed`, and the pager's actual `disk budget` and `device free`. A Windows `DRIVE_FIXED` value does **not** establish that a USB HDD is internal. If UWP denies the bus query or reports a virtual/unknown bus, that drive is conservatively excluded; D: may therefore be rejected and Q: used. Console verification is essential. |
| 2. Allocation admission | The log records every >=16 MiB DeviceLocal allocation that would cross `limit - 250 MiB` and every proposed dense merge whose projected live total would exceed the cap. A merge's full new allocation is included in the pre-allocation peak; old arenas have not retired yet. Dense buffer construction is outside `dense_arena_mutex`, avoiding re-entering the same mutex when an allocation recovery asks for retired arenas. | **Not done:** no actual cap, refusal, `Finish`/`PopPendingOperations` recovery ladder, smaller arena or skipped merge. `BufferCache::ObtainBuffer` and readback paths require a valid contiguous `Buffer*`; splitting or returning null without propagating failure through draw/dispatch/readback and DMA mappings risks corrupted GPU data. Do not read the observed would-refuse count as memory saved. |
| 3. Watchdog | With `watchdog_fast=1`, usage is sampled every 250 ms, with full checks at 250 ms inside 600 MiB headroom and 1 s outside. Crossed rungs are queued in order, with a separate serialized worker, so a blocked reclaim cannot stop the monitor from deciding to exit. The new opt-out-able reduce rung requests 640x360 scene proxies and 720p output; it does not claim those settings immediately freed memory. | `UWP watchdog: step ... queued/started/freed ... MiB`; a rung that freed zero logs zero. A live resolution request requires another rendered frame and may be overridden by startup-patched `BB_RENDER_RES`. The exit prints a lock-free pager state and breadcrumbs; a separate worker attempts save flush and remaining reports for up to five seconds. A timeout explicitly says `save flush NOT confirmed`. |
| 4. Compressed pool | Use `VirtualFree(..., MEM_DECOMMIT)`, not `VirtualAllocFromApp(..., MEM_DECOMMIT)`. Track commitment per 4 KiB page, including failed decommits and subsequent reuse; decrement live pool pages on page-in/replacement; calculate savings against actual committed bytes. Free extents merge before checking whether the descriptor table is full. | `UWP swap pool ... committed ... decommit failures (last error ...) ... actually decommitted`, and the memory table's `pool ... saved`. New Windows CI exercises decommit/recommit and verifies a neighbouring live page survives; that is not a Series S/X test. |
| 5. Textures | `texture_reclaim_experiment=1` opts into a PR #89-style **experimental discard** during GC (including its emergency pass). It considers old GPU-written tiled targets only under critical pressure, after bound, target, scene-proxy, video-out, collected and persistent id/uid pin checks. The camera's stored depth id is paired with its uid and re-resolved with `TryGetImageTouched`; the Xbox upscaler implementation is a stub and stores no ids. | Off by default. `UWP texture experiment ... scheduled ... KiB` is only a scheduled allocation; `... released ... KiB host allocation after GPU fence` is the VMA allocation destroyed, not necessarily an equal drop in OS usage. A stale/unpinned GPU-written surface may still be needed by the guest; its contents are discarded without a tiled write-back. **Safety and visual correctness are unverified.** The separate off-thread `EmergencyReclaim` path deliberately does not perform these experimental drops. The existing explicit `BB_GC_DROP_OLD` override is independent and remains off by default. |
| 6. Resolution | Added measurements for actual VMA sizes of native and proxy targets. Source inspection shows `scene_res` controls host `SceneTargets` proxies; it does not resize the retained native guest targets. `output_res` drives the window/presentation dimensions and the bridge reports its actual shared-surface allocation. A startup `BB_RENDER_RES` patch can override live scene scaling. | Compare `Camera motion: scene render size` (guest camera), `UWP resolution: native target ... allocation ... retained; proxy ... allocation ...`, and `CoreWindow presentation bridge ... shared image ...`. Fewer raster pixels do not prove a lower overall allocation: native images and proxies can coexist. |
| 7. First vs warm | The header checks real `Prepared/boot-linked.bin`, `shader_cache`, and `user/cache` paths before creating Prepared or importing caches; it prints existence, counts and bytes. A lightweight usage/presents curve is emitted throughout the run, marked cold/warm/mixed. | `UWP cache launch` and `UWP run curve` for one cold and one warm run over the same route. The provided summary establishes that .813 had fresh caches, but provides no comparable warm-run memory curve, so no cold-vs-warm difference can yet be measured. |

A bus query can be denied by an Xbox AppContainer even when a root-level file probe succeeds.
In that case the safety rule wins over choosing an unverified volume; the log says exactly why it
fell back. The 64 MiB speed test is skipped when it would consume the configured reserve. The swap
file's slot encoding supports at most 2 GiB of disk records, so larger configured budgets are
explicitly capped to 2,048 MiB.

## Tests and next console runs

The repository's `xbox/tests` set is run locally where possible and by the GitHub Actions Xbox
package job. The broader Python suite on this Linux sandbox ran 297 tests with 20 errors and 65
skips: its Windows launcher imports require `ctypes.WINFUNCTYPE`/`ctypes.windll`, and Pillow is not
installed. Those errors are outside the Xbox changes and are not represented as passing. New tests cover volume selection (including a USB disk that reports fixed media),
watchdog thresholds/queued order, pool commitment bookkeeping, the Windows decommit API, texture
pin/drop predicates and overflow-safe admission arithmetic. The Windows unit test exercises Windows
virtual memory, not the Xbox app container.

Run once with the defaults and once warm over the identical route. For the experimental texture
comparison use `texture_reclaim_experiment=1` only as a separate, explicitly opt-in run; a black
screen, missing shadows or incorrect image contents is a failure, not an acceptable saving. Preserve
`loader.log.txt` and `errors.log.txt` immediately after a stall or crash, before relaunching. No
result should be called fixed until the corresponding Series S/X test passes.
