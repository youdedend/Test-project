# release-notes-diag1.md — diag-1 (build on top of xbox-alpha-1.0.26314.1455 / `9849caf`)

Prepared 2026-11-10 by the research session (read-only on Test-project). Base:
`arena/c40c18ec-test-project` @ `9849caf`. Prepared tip in the bundle: `eb3ad91`
("diag-1: staging telemetry row, wedge tags, and copy-mismatch hardening"). No `[release]`
in it — the marker commit is the publisher's.

## What diag-1 contains (8 files, +140 lines, no behavior change outside the BB_UWP skip path)

1. **Staging telemetry row** (answers the step-1 measurement questions Q1/Q2 that the 2026-11-10
   hardware run could not): `StagingBufferPool::GetStats()` reports o1heap arena bytes/chunks/live
   records vs legacy dedicated large buffers; 5 fields added to BOTH `BbGpuMemoryCats` copies
   (`gpu/bbgpu.h` + `xbox/platform/main.cpp`), filled in `bbgpu_uwp_memory_cats`, printed as a
   `staging:` row under the `arena:` row in the 5 s memory table.
2. **Wedge tags** (`bbgpu_thread_op`, named in the existing `UWP thread op:` stall dump):
   `dense-arena merge copy` (xbox/graphics/dense_buffers.cpp merge loop), `copying image (texture
   transfer)` (Runtime::CopyImage, RAII-cleared), `buffer-cache allocation`
   (buffer_cache/buffer.cpp UniqueBuffer::Create), `copy layer mismatch - skipping copy`.
3. **Copy-mismatch hardening** (vk_runtime.cpp SanitizeCopyLayers, BB_UWP): every skip is counted;
   a (src,dst) mapping pair that mismatches twice is poisoned and later copies to it are refused
   with a throttled log. Root-cause-informed (2026-11-10 decode pinned the character-load crash to
   `dzn_image_get_copy_loc()` reading `device->physical` through a stale image object) but the
   attribution is NOT proven — this refuses rather than clamps, and every refusal is visible.

## Additive public API (flagged to the user; approved implicitly)

- `StagingBufferPool::GetStats()` + nested `Stats` struct (new, read-only).
- 5 new fields in `BbGpuMemoryCats` (internal struct, both copies kept field-for-field identical).

## What to measure on the next hardware run

- `staging:` row values (Q1 staging MiB; Q2 arena vs dedicated split) — first-ever visibility.
- At any wedge: the `UWP thread op:` dump now names merges / texture copies / big allocs.
- `GPU: copy layer mismatch skip #N (poisoned pairs: M)` and `GPU: refusing copy to poisoned
  mapping pair` lines: do the crash-window mismatches come from one mapping pair?
- Optional zero-code aid: `DZN_DEBUG=d3d12` in `graphics-debug.txt` (dzn fork cc9399e).

## Host tests (run before publishing)

`dense_arena_lifetime`, `arena_admission_policy`, `recovery_ladder`, `staging_arena` — pass
(g++ recipe locally; CI runs the full `xbox/tests` table on the build).
