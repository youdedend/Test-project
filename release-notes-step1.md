# Phase 1/2a + item-2 step-1 — what to test on hardware

Commit being pushed: `d31be49` (contains `681384b` 2a recovery ladder + the o1heap staging
arena). Same route as the plan: fixed regression route (character creation → Yharnam →
Cleric Beast area → Hunter's Dream → 3 memory transitions → 20-minute soak), one variable
per run, logs BEFORE relaunch after any crash/stall (loader.log.txt + errors.log.txt only).

## What step 1 changed

The staging pool's large/deferred path (`gpu/shadps4/video_core/renderer_vulkan/
vk_staging_buffer_pool.cpp:170`) now sub-allocates from o1heap arenas (uploads =
HostUncached, readbacks = HostCached). A freed region serves a later request of ANY size
once its GPU tick is free; the old best-fit-in-1.25x window could not. Safety rule kept:
nothing is reused before `scheduler.IsFree(tick)`, deferred allocations release only via
`FreeDeferred` (`vk_staging_buffer_pool.cpp:233`). Any arena miss falls through to the
legacy dedicated path unchanged — worst case the behavior equals the previous build.

## How to measure the win (T13/T21-style comparisons)

1. **staging bytes**: `ResidentBytes` / the `resident_image_rows` staging lines in
   `errors.log.txt` (now include arena chunks). Compare peak MiB across the 3 memory
   transitions vs the same run on `.1804`.
2. **allocation churn**: count `"StagingBufferPool:Dedicated"` buffers vs
   `"StagingBufferPool:Arena"` chunks in the log; step 1 replaces many dedicated buffers
   with few chunks.
3. **faults**: the same +0x3003f0 / +0x26bbb7 / +0x27222e / +0x272f1c RVAs and
   DEVICE_LOST events at the 5,055-5,116/5,120 MiB wall. Fewer arena failures = fewer
   late OOM paths = fewer of these; that is the hypothesis to confirm or kill.
4. **T21 burst**: +447 MiB per 2-present burst — watch whether the burst still spikes
   staging bytes the same way (the arena consolidates address space, it does not
   magically shrink working sets — measure honestly).

## Rollback

`staging_arena=0` in `bbport.ini` (or `BB_STAGING_ARENA=0` in graphics-debug.txt) restores
the legacy staging path exactly. The 2a recovery ladder (`arena_admission_recover=1`)
is independent and stays on.

## Bounds (honest limits)

- o1heap rounds each allocation up to a power-of-two span (vendored `o1heap.c:388-390`), so
  a request can consume up to 2x its size internally; chunk sizing budgets for it, but
  compare MiB, not allocation counts alone.
- No paging, no admission enforcement (2b), no fragmentation compaction — this step only
  fixes reuse of freed staging regions.
- Host tests: `staging_arena: PASS` (tick safety, deferred-held, cross-size reuse,
  alignment, growth, chunk release, fallback) + all earlier rows unchanged; the
  `xbox.yml` run confirms on the real toolchain.
