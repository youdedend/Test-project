# Master plan, BUILD 3: merge accounting, fail-soft allocation, the recovery watchdog, feature costs

Three builds, flag-gated, each measured. This document is BUILD 3 only (plan items 13-16). Nothing
here has been verified on a console: every "measured as" line is the number the code prints. **No
claim in this document is a fix until a Series S/X run passes.**

Evidence this build answers to: `819` reached 5,116/5,120 MiB at world load with `arena: live=912
retired=448 MiB`, textures at 301 MiB and 1,149 MiB of swap gone, then stopped presenting at 877. The
log could not say how the arenas got that large, what the allocation ladder's last rung was, or which
step had already been tried — and the app was killed rather than exiting with its saves flushed.

## Item by item

### 13. Every overlap merge is logged — `xbox/graphics/dense_buffers.cpp:83-140`

A dense arena is replaced by a larger one whenever a buffer is bound across a range that spans more
than one existing arena: the arenas expand until the range is covered, their contents are copied into
one new arena, and the old ones are retired (still held until the replacement tick is signalled). That
is correct, but it was silent, and it is the largest single allocation the app makes — one 64 KiB
request can turn four arenas into one of several hundred MiB, held twice while the retirement lasts.

One line per merge:

```
UWP dense GPU arena: merge #7 0x1a00000000: 4 arena(s) 612 MiB -> one of 704 MiB, for a buffer at
0x1a01000000-0x1a01ff0000; copied 612 MiB, cumulative 1,940 MiB in 7 merge(s), largest 704 MiB
```

and, for the evaluation item 13 asks for, a second line whenever the result is over
`BB_ARENA_MERGE_CAP_MB` (default 256, `0` disables):

```
UWP dense GPU arena: merge #7 is 704 MiB, over the 256 MiB cap: splitting the range would need 3
arena(s) of this size (and one DMA view slot per page, 176 available)
```

**Measured as**: `cumulative N MiB in M merge(s), largest N MiB` (how the ~928 MiB arenas of `819`
were built), and the cap line's `would need N arena(s)` for the split cost.

**Why nothing is actually split yet**: the page table encodes a page as `(slot + 1) << 32 | offset`,
so a page's arena must stay contiguous, and each arena page costs one of a fixed 176 DMA view slots
(`xbox/graphics/dense_buffers.cpp:41` asserts on overflow). Splitting is a change to both, and the
first thing to establish is whether a merge on the real route is ever above the cap — that is what
the log line answers.

### 14. Fail-soft allocation: wait for the frees already in flight, then degrade — `buffer.cpp:44-90, 262-292`

The ladder was: page out → free images → assert. Every rung asked once. Both `.736` and `.819` show
why that is not enough: a retired-arena release is deferred while a draw-prep producer is in flight
(always, at world load), and the image walk runs under the cache mutex while another thread inserts —
so memory that becomes available a frame later is memory the failed allocation never sees.

Three changes, in order:

1. **Wait for the frees in flight** — `bbgpu_uwp_wait_for_release(need, timeout_ms)`
   (`gpu/shim/bbgpu.cpp:528`). It re-asks the release every 20 ms until the need is met or the
   deadline passes, and prints what the waiting bought, including the retired-arena total either side:
   `UWP memory: waited 180 ms for in-flight frees (9 ask(s)): released 96 MiB of the 128 MiB wanted;
   retired arenas 448 -> 352 MiB`. Budget `BB_ALLOC_RETRY_MS`, default 250 ms, clamped to 0-2000.
2. **Degrade instead of stopping** — before the retry, step 3 of the watchdog's ladder runs once
   (`bbgpu_uwp_recovery_step(3, 0)`): optional features off and the internal resolution down a notch,
   so the *next* attempt needs less (`BB_ALLOC_DEGRADE=0` disables). Capped at four log lines: an
   allocation loop that degrades the picture on every failure is a bug, not a recovery.
3. **Then fail** — the third attempt still ends at the unmodified `ASSERT_MSG`. Nothing is weakened to
   get a green run; a failure that survives every rung still stops, and still logs why.

**Measured as**: the two lines above, plus the existing
`UWP memory emergency (allocation failed): need N MiB; paged out N MiB in N ms, freed N MiB of images
in N ms`, which now includes the wait in its second number.

### 15. The recovery watchdog — `xbox/platform/watchdog_policy.h`, `xbox/platform/main.cpp:1833-1920, 2760-2870`, `gpu/shim/bbgpu.cpp:587`

Its own thread, a 1 s check, a cooldown per step (2 s), every step logged with MiB freed. The rules
live in a header with no threads and no Win32 (`watchdog_policy.h`), so the CI test covers them:

| Step | Trigger | What it does | Where |
| --- | --- | --- | --- |
| (a) release | headroom ≤ 400 MiB | release retired arenas, trim idle staging, then any image the collector may take | `bbgpu_uwp_recovery_step(1, need)` |
| (b) pager | headroom ≤ 250 MiB | forced pager sweep, compressed pool included | `bbgpu_uwp_recovery_step(2, need)` |
| (c) degrade | headroom ≤ 150 MiB, or presents stuck 3 s | upscaler off, frame generation off, optional buffers (reactive mask, object motion) off, output → 1280x720, preset one notch down | `bbgpu_uwp_recovery_step(3, need)` |
| (d) exit | presents stuck 10 s **and** headroom ≤ 150 MiB | dump pager/memory/files/waits/threads, breadcrumbs, `runtime_files_flush()`, then `close_app` with the reason | `main.cpp:2830-2860` |

```
UWP watchdog: on; ladder 400 MiB, pager 250 MiB, degrade 150 MiB, stuck 3 s degrades / 10 s exits,
cooldown 2000 ms            (printed once, from bbport.ini)
UWP watchdog: step release (arenas, staging, images) freed 112 MiB in 38 ms (headroom 355 MiB of
5120, usage 4765/5120 MiB, presents 861, stuck 0 ms)
UWP recovery: released 112 MiB (arenas 96, staging 0, images 16 MiB) of the 45 MiB wanted
```

* **Order matters and is asserted**: the exit is decided first but needs both halves (a 10 s stall
  *and* no headroom) — a 10 s stall with memory to spare is a bug whose evidence the stall report
  should keep. (a)/(b) repeat once per cooldown; (c)/(d) are one-shot escalations and are never held
  back by the cooldown, so a run cannot spend the exit threshold on a resolution change.
* **Limit changes**: when `AppMemoryUsageLimit` moves mid-run (another app suspended, the game left
  the foreground), the watchdog re-checks immediately, resets the cooldown and runs (a) if the new
  limit puts the app within the ladder threshold.
* **Trim staging is the safe trim**: `StagingBufferPool::TrimIdle()`
  (`vk_staging_buffer_pool.cpp:244`) applies exactly the rule the per-frame trim applies (a ring block
  beyond the ring's keep count and idle 120 frames; a large buffer idle 120 frames and signalled). An
  urgent one-idle-frame trim destroyed blocks that recorded commands still referenced — that is why
  it is the same rule, run now.
* **Thresholds in `bbport.ini`**: `watchdog=`, `watchdog_ladder_mb=`, `watchdog_pager_mb=`,
  `watchdog_degrade_mb=`, `watchdog_stuck_s=`, `watchdog_exit_s=`, `watchdog_cooldown_ms=`. The file
  is migrated to `settings_rev=3` (only additive lines; no existing line is touched) so a console that
  has been running since before this build gets them without a wipe.
* **Saves are flushed, not committed**: `runtime_files_flush()` (`src/runtime_file.c:874`) fsyncs the
  open, dirty `/savedataN` files and leaves them open. A commit is a rename the guest asked for; this
  only makes sure bytes already written are on the volume before the app closes.

**Measured as**: `step <name> freed N MiB in N ms` for (a)/(b); for (c) the next usage line (the
feature targets it released are visible as the gap between `app_usage` before and after); for (d) the
dump plus `flushed N open save file(s)`.

### 16. Feature byte costs, the 50 MiB rule, the frame cap and the caches — `main.cpp:1788-1810, 1887-1945`

* **Cost, before anything else**: each optional feature's host targets are computed from its own
  resolution (4 bytes per pixel per target) and printed whether or not it is on:
  `UWP feature budget: output 1280x720 -> scene target 3.5 MiB; upscaler=off costs 0.0 MiB,
  frame_gen=off costs 0.0 MiB; features over 50 MiB are turned off`.
* **The 50 MiB rule**: a feature that is on and costs more than 50 MiB is turned off, and the line says
  what it would have cost and how to keep it (`heavy_features_optin=1` in bbport.ini). The override
  goes through the environment (`BB_UPSCALER` / `BB_FRAME_GEN`), because that is how the settings
  loader applies a forced value after the file — the player's `bbport.ini` is not rewritten, so their
  intent stays in the file. At 720p no feature reaches the threshold; at 1080p the upscaler's history
  pair is ~16 MiB, and frame generation at 4K is ~100 MiB, which is exactly the case the rule exists
  for.
* **Defaults**: 720p, upscaler off, frame generation off were already A5's console defaults
  (`bbport_settings.h:52-56`); this build adds the enforcement for a file written by the PC launcher.
* **The 30 fps cap, evaluated**: `fps_cap=` is printed with the scene-target size and the note that a
  30 cap halves the frames the GPU may queue. It is *not* changed behind the player's back — a cap
  cannot be measured into a log line the way a MiB can, and the plan asks for an evaluation.
* **Downscale-textures, evaluated — and not implemented**: there is no texture-downscale switch in
  this tree (checked: nothing in `gpu/` or `xbox/` reads one). The measurement that exists is the
  per-minute memory line's `textures N (M MiB)`, and the only lever that changes it today is
  `scene_res=` / the preset, which changes what the *game* uploads rather than how the cache stores
  it. A real downscale would have to intercept image creation and re-create every guest texture at
  half size, which changes what the guest's own render targets sample — a corpus-level change that
  must be measured on hardware, not added behind a flag without one. Reported as not done.
* **Shader/pipeline cache sizes**: `UWP caches: driver.vkcache=N MiB prepared.bin=N MiB shaders.bin=N
  MiB` at the top of every run, measured before the game starts, so the size is what the previous run
  left behind. **Shrinking an over-provisioned cache is not done in this build**: the only honest
  threshold is "large and never read", and the run header is what makes that visible.

## What is flag-gated

| flag | default | what it does |
| --- | --- | --- |
| `watchdog` (ini) | 1 | every step of the recovery watchdog |
| `watchdog_ladder_mb` / `_pager_mb` / `_degrade_mb` (ini) | 400 / 250 / 150 | the three byte thresholds |
| `watchdog_stuck_s` / `_exit_s` / `_cooldown_ms` (ini) | 3 / 10 / 2000 | the stall steps and the cooldown |
| `heavy_features_optin` (ini) | 0 | keeps an optional feature over 50 MiB |
| `BB_ALLOC_RETRY_MS` | 250 | the bounded wait for in-flight frees (item 14) |
| `BB_ALLOC_DEGRADE` | 1 | degrade once before an allocation gives up |
| `BB_ARENA_MERGE_CAP_MB` | 256 | the size above which the merge log evaluates a split; 0 disables |

## Tests

`xbox/tests/watchdog_policy_test.cpp` (new, run by the `Dozen for UWP` CI job): threshold defaults are
bytes not MiB, the step order at every boundary (400/250/150 MiB and the two stall times, including
that a 10 s stall with memory to spare does not exit and that the exit outranks the degrade), the
disabled and unknown-limit cases, the cooldown (first step not delayed, inside/after, a backwards
clock), `ShouldRun` for repeatable vs one-shot steps and escalation past the cooldown, and the ini
parsing (fallback on a missing key, a word, a trailing unit; clamping at both ends).

`xbox/tests/texture_cache_policy_test.cpp` (Build 2) still passes, and the other platform tests are
unchanged.

Locally: `g++ -std=c++20 -Wall -Wextra -I xbox/platform xbox/tests/watchdog_policy_test.cpp` — clean,
all checks pass.

## What this build does not do

* **It does not split arena merges.** Item 13 asks to *evaluate* capping the merge size, and the split
  needs contiguous pages (the page table encodes `(slot + 1) << 32 | offset`) and one DMA view slot
  per page out of a fixed 176. The log line says what a split would have cost on the real route; the
  change waits for that number.
* **It does not downscale textures.** No such switch exists in this tree; see item 16 above.
* **It does not shrink an over-provisioned pipeline cache.** The sizes are reported at every run;
  shrinking without a measurement of what is re-read would trade a save for shader-compile stutter.
* **It does not change the 30 fps cap.** `fps_cap=` stays the player's; the line prints what a cap
  would mean.

## What needs the console

* Whether a merge on the real route is ever over 256 MiB (the split evaluation is only worth acting on
  if it is), and what the cumulative merge total is over a session.
* Whether the bounded wait is enough at world load: the line to read is
  `waited N ms for in-flight frees ... released M MiB`, next to the emergency line's totals.
* Whether the watchdog's (a) runs *before* the allocation failure at all — if every run shows
  `step release ... freed 0 MiB` at 400 MiB of headroom, the threshold is too early and the plan's
  number should be reconsidered on the log, not guessed again.
* Whether (c) is ever reached, and if so, what the picture does — the degrade path changes the
  resolution live, and only a console run can say whether that is better than the stop it replaces.
* Whether (d) exits with the saves intact: the after-the-fact check is that the save file's contents
  are the ones from before the stall, and that `flushed N open save file(s)` is non-zero.
