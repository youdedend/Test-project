# Master plan, BUILD 2: the texture-cache collector

Three builds, flag-gated, each measured. This document is BUILD 2 only (plan items 8-12). Nothing
here has been verified on a console: every "measured" line below is the number the code prints, and
the checks that passed are named. **No claim here is a fix until a Series S/X run passes.**

Evidence this build answers to: `0717` reached its critical mark and the emergency reclaim — the pass
that ignores the age gate — reported `seen=... gpu-written=... bound/target=...` with nothing freed,
and the log could not say which rule did it. `819` then stalled at 5,116/5,120 MiB with textures at
301 MiB and 1,149 MiB of swap gone.

## Item by item

### 8. A blocked image costs nothing and keeps its place in line — `texture_cache.cpp`, `texture_cache_policy.h`

Before this build a candidate the collector refused still returned from the LRU walk without being
touched in some paths, and only one of the four refusal branches was counted. Now:

* every refusal is classified at **one** decision point, `BbGpu::GcClassifyCandidate`, in a fixed
  order: `pinned` → `gpu_written_tiled` → `unsupported_layout` → `not_pressured` →
  `writeback_budget` → `none`;
* a refusal spends **no** deletion (`num_deletions` is decremented only on an actual free), so a run
  of unevictable entries can no longer stall the pass: the old `unused 16/4/2 s` rounds hit a wall
  of render targets and stopped for the session;
* a refused candidate is `Touch`ed to `gc_tick`, so `below_tick` no longer selects it and the next
  pass starts past it instead of on it;
* the per-pass line names the columns:

```
Texture cache: collector pass 42: examined=311 freed=7 (18 MiB), blocked: pinned=2 tiled-gpu-written=280
writeback-budget=0 not-pressured=22 layout=0; written back=0 (failed 0), depth=0, dropped=0
```

`examined == freed + every blocked column`, so a pass that freed nothing says why. When the third
(the age-ignoring) pass frees nothing the line carries the suffix `-- the emergency pass (the one that
ignores age) freed nothing; the columns above are why`. That sentence is the answer `.717` was
missing — and on the evidence so far the expected answer is `tiled-gpu-written`, i.e. every candidate
was a GPU-written tiled image, which needs item 12's tile-manager reverse path, not a bigger budget.

Line budget: at most 400 report lines per session, plus the once-a-minute evict/insert line.

### 9. Depth and shadow reclaim, behind every pin — `texture_cache.cpp`, `BB_GC_DEPTH_RECLAIM`

Depth/stencil targets and shadow maps are GPU-only (`SafeToDownload() && IsTiled()`), so the keep
rules retained them for the whole session and VRAM climbed with every area visited. Under pressure
(and only under pressure) a depth image is freed outright — nothing in it is worth writing out, and
being tiled it could not be written out anyway. This is Mrsuss60/bloodborne_pc_windows_port PR #89,
whose issue #69 is our +200-500 MB per zone load and whose #75 is our `ErrorOutOfDeviceMemory`.

The pin gate runs **before** the depth rule and before every free, and it is unchanged from the
crash-free order: `bound`, `target`, `scene_proxy`, `vo_surface`, `collected` (the rasterizer's
`HoldCollectedImage` set), plus the two holders that re-resolve instead of indexing: `CameraMotion`
(`depth_id`, via `TryGetImageTouched`) and `TemporalUpscaler` (`scene_color`, `ldr_target`,
`ui_color`, `ui_depth`, via `TryGetImageTouched` + uid). A held image is touched, so it is fresh; and
if it is reached anyway the free path still cannot hand a dead id back to a holder — the holders
check `is_allocated`/uid and skip their frame. **This is the stale-id crash and nothing in this build
changes the order that prevents it.**

`BB_GC_DEPTH_RECLAIM=0` turns the rule off for an A/B run; it is on by default.

### 10. Stale tiled GPU-written images — `BB_GC_DROP_OLD`, off on UWP

`BB_GC_DROP_OLD=1` lets an image that is tiled, GPU-written and untouched for ~45 s go **unwritten**
at the critical mark. It is off by default everywhere and off on the console by policy: a tiled
target the game still reads loses its contents (black screen with sound, 6 GB cards, entering the
Hunter's Dream), and GoW3 on the same console family has no tiled-drop path at all.

New in this build: the switch is no longer passed to the classifier as a switch. The classifier gets
**this candidate's decision** (`drop = drop_old && tiled && !dirty && aggresive && older than a
minute`). Passing the switch would have let a tiled image the age rules did not select fall through
to the write-back below — and `DownloadImageMemory` copies linearly, so it would have written a linear
layout into the guest's tiled surface. Only an image actually being dropped bypasses the tiled rule.

Measure both, on one session each:

1. `BB_GC_DROP_OLD=0` (default): the 5 s pressure line's `N old GPU-written tiled images kept (X MiB)`
   and the per-pass `tiled-gpu-written=` column give the cost of keeping them. X is the number to
   report.
2. `BB_GC_DROP_OLD=1`: the same two numbers plus `dropped=`, which is the MiB actually reclaimed. If
   the frame-rate drops or the screen goes black in a new area, the drop is the cause and the default
   stays off.

### 11. Anti-thrash — hysteresis, back-off, write-back budget, guarded free

* **Hysteresis** (`GcHysteresisActive`): the collector starts at `trigger_gc_memory` and does not stop
  until usage is below `trigger - 128 MiB` (`BB_GC_HYSTERESIS_MB`). Without the gap a run sitting on
  the mark evicted and re-uploaded the same textures every submission.
* **Back-off** (`GcShouldBackOff`): a pass that freed under 32 MiB and has seen under 64 MiB of growth
  since the last one is skipped, because walking the whole cache under the mutex cannot find what it
  did not find a moment ago. `collector backing off (last pass freed N MiB, usage grew M MiB; K
  passes skipped)` is printed once per 200 skips.
* **Write-back budget**: at most 4 synchronous write-backs per pass (`BB_GC_MAX_WRITEBACKS`, clamped
  to 32). Each one finishes the GPU and copies over the bus; four is already a frame or two of stall.
* **Reclaim only after a successful write-back**: `DownloadImageMemory(image_id, true)` now returns
  `bool` (false = the guest write-back was refused, `TryWriteBacking`). A refusal keeps the image,
  counts `writeback_failed`, and costs no deletion. Before this, the image was freed whether or not
  the copy landed — the same loss the Windows port reports as a black screen with sound.
* **Evict/re-upload cycles per minute**:

```
Texture cache: per minute: 37 image(s) evicted, 40 image(s) inserted (inserts well above evictions
means the cycle is eviction, not load)
```

  A texture evicted and uploaded again every pass moves no MiB number, so it needed its own line.

### 12. Upstream audit — report only, nothing merged

Asked whether shadPS4 PR #3396/#3374 and the BufferCache GC leak fix (#3787, `038238d`) are in this
tree.

* **#3374 (tilers)** — *present, in the form this fork needs.* `video_core/texture_cache/tile_manager.{h,cpp}`
  has `DetileImage` (guest tiled → host linear) and `TileImage` (host linear → guest tiled, via a
  compute dispatch with the guest's `TilingInfo`), and `BufferCache::SynchronizeMemoryFromImage`
  (`buffer_cache.cpp:799`) already calls `TileImage`, so the reverse path exists and is exercised.
* **#3396 (garbage collector part 2)** — *partly present, and the missing part is the one that
  matters for item 8.* #3396's point is that a GPU-modified **tiled** image can be downloaded at all:
  tile it back into the guest layout instead of retaining it forever. Our `TextureCache::DownloadImageMemory`
  (`texture_cache.cpp:145`) is still the linear-only copy: it builds one `vk::BufferImageCopy` with
  the image's pitch and hands it to `runtime.DownloadImage`. Feeding it a tiled image would write a
  linear layout into a tiled surface — which is why the collector now refuses `gpu_written_tiled`
  outright rather than trying. Porting cost: small and local — build `mips_layout`-based
  `buffer_copies` the way `SynchronizeMemoryFromImage` does, allocate the staging buffer, call
  `TileManager::TileImage` into it, then `TryWriteBacking`; roughly 60-80 lines plus a fallback for
  the layouts `GetTilingPipeline` cannot build. Risk: **medium-high** — every block here is a
  guest-visible surface, the compute pipeline set is per-format, and a wrong `TilingInfo` is silent
  corruption rather than an assert. It also invalidates the buffer cache's copy of the same range
  (#3396 does this explicitly: "Invalidate buffer cache memory on image download"), which we would
  have to add too. Not done in this build: it is a change to what the guest reads back, and it needs
  a console to falsify.
* **#3787 / `038238d` (BufferCache GC never ran)** — *not applicable, and the leak class it fixes is
  already covered differently.* The upstream bug is that `BufferCache::RunGarbageCollector` built its
  cleanup lambda and never called `lru_cache.ForEachItemBelow`, so per-buffer host memory was never
  freed. This fork has no per-buffer LRU at all: buffers live in sparse **arenas**
  (`buffer_cache.h`, `MAX_ARENA_PAGE_BITS`), and the equivalent path is arena retirement and
  `ReleaseRetiredArenas()` (`buffer_cache.cpp:805`, called from `SubmitPendingArenaBinds`). The
  upstream fix is 3 lines and does not apply; the risk it describes is Build 3's item 14/15 (releasing
  retired arenas under the watchdog), which is already planned.

**Nothing from #3396/#3374/#3787 was merged, per the plan.**

## What is flag-gated

| flag | default on UWP | what it does |
| --- | --- | --- |
| `BB_GC_DEPTH_RECLAIM` | on | free depth/shadow targets under pressure (item 9) |
| `BB_GC_DROP_OLD` | **off** | drop an old tiled GPU-written image unwritten (item 10) |
| `BB_GC_HYSTERESIS_MB` | 128 | trigger-to-target gap (item 11) |
| `BB_GC_MAX_WRITEBACKS` | 4 | synchronous write-backs per pass (item 11) |
| `BB_GC_IDLE_SECONDS` | 8 (UWP) | "unused" for a normal pass |
| `BB_GC_PRESSURE_IDLE_SECONDS` | 2 (UWP) | "unused" above the pressure mark |
| `BB_GC_PRESSURE_PERCENT` | 60 | pressure mark as a fraction of the budget below 8 GB |

## Tests

`xbox/tests/texture_cache_policy_test.cpp` (built and run by the `Dozen for UWP` CI job) now covers,
for the new rules and with the header alone — no Vulkan, no Windows headers:

* `TestClassificationOrder` — an unremarkable candidate is freeable; a pin outranks every write-back
  rule; a GPU-written tiled image is retained; `BB_GC_DROP_OLD` releases it.
* `TestUnsupportedLayout` — an inexpressible layout is kept, and the drop switch does not bypass it
  (eFMann PR #18).
* `TestWritebackBudgetAndPressureGate` — below the mark the candidate is `not_pressured` even with the
  budget spent (order is asserted); the last write-back of the budget is allowed, the fifth is not; a
  budget of zero does not block an image nobody has to copy.
* `TestBackOff` — both sides of 32 MiB freed / 64 MiB growth.
* `TestHysteresis` — start at the trigger, keep going below it, stop at trigger − hysteresis, do not
  restart half-way up the band, no underflow when the trigger is smaller than the gap.
* `TestPassAccounting` — a mixed population of ten candidates, asserting `examined == freed + every
  blocked column` and that the candidate past the budget is counted as such.

Locally: `g++ -std=c++20 -Wall -Wextra -I gpu/shadps4/video_core/texture_cache
xbox/tests/texture_cache_policy_test.cpp` — clean, all checks pass.

## What needs the console

* Whether the emergency pass's blocked column really is `tiled-gpu-written` on `.717`-shaped stalls.
* The `BB_GC_DROP_OLD=0` vs `=1` measurement above (kept MiB, dropped MiB, frame rate, and whether a
  new area renders).
* Whether hysteresis and back-off change the evict/insert ratio in the per-minute line — the
  hypothesis is that inserts fall to roughly the evictions instead of doubling them.
* Whether depth reclaim alone is enough to keep an area transition under the pressure mark; if VRAM
  still climbs, item 8's answer plus #3396's port is the next step, and it is a corpus-level change
  that must be measured on hardware, not argued about here.
