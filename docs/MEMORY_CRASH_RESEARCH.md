# The crash at high GiB — what the community found

Research round, 2026-10-27. Question: Bloodborne (bbport family) dies once memory use climbs into the
high-GiB range — "Texture cache: memory pressure, N of M MiB", then the GPU stops — and other people
have hit it, so what did they find?

## 1. Somebody hit exactly this, with our log lines

`Mrsuss60/bloodborne_pc_windows_port` is a sibling of this project (same shadPS4 texture cache, same
bbport collector, same log format). Its **#75 "game crash due to vram leak (1650 super)"**, now closed,
contains this:

```
Texture cache: memory pressure, 3866 of 3729 MiB (critical 3936): 115 images evicted, 0 written back since the last report
Texture cache: memory pressure, 3878 of 3729 MiB (critical 3936): 146 images evicted, 0 written back since the last report
Texture cache: memory pressure, 3889 of 3729 MiB (critical 3936): 118 images evicted, 0 written back since the last report
GPU [Debug] <Critical> vk_platform.h:66 Check: Assertion Failed!
vk::Result=ErrorOutOfDeviceMemory
STOP: GPU library assertion failed (see GPU log above)
Process finished with code: 23
```

That is our failure: usage parked just under the critical mark, the collector evicting images every
report and freeing nothing that matters, then `ErrorOutOfDeviceMemory`. Reporters: 4 GB VRAM laptops
(RTX 3050 4 GB, GTX 1650 SUPER), RX 6600M 8 GB, all "smooth 60 FPS for 15–30 minutes, then this".

Companion issue **#69 "Unbounded VRAM/RAM accumulation during map transitions"**: "host process
footprint steadily expands by 200 MB–500 MB per zone load… VRAM increases continuously until
saturating hardware limits… eventually causing a crash with VK_ERROR_OUT_OF_DEVICE_MEMORY".

Both were closed by **PR #89 — "gpu/texture_cache: Reclaim transient depth targets and mitigate VRAM
pressure leak"**, which changes 13 lines in `gpu/shadps4/video_core/texture_cache/texture_cache.cpp`
and states the cause precisely:

> 1. **Depth/Stencil & Shadow Maps**: GPU-modified depth attachments have `SafeToDownload() == true`
>    and `IsTiled() == true`. Because tiled downloads cannot be written back, they are **touched and
>    kept alive forever (lru_cache.Touch), locking gigabytes of VRAM**.
> 2. **Permanent Tiled Retention**: When tiled && download textures are encountered, they are never
>    freed even under active memory pressure (`pressured == true`), causing VRAM to stay pinned at the
>    pressure mark.

and the fix:

> 1. **Direct Depth Reclaim**: if `image.info.props.is_depth`, the image is a GPU-only depth/stencil or
>    shadow map. Guest CPU code never reads depth maps back into host RAM. We directly reclaim it via
>    `FreeImage(image_id)`.
> 2. **Pressure-Aware Eviction**: under memory pressure, stale tiled images that have aged past the
>    horizon are evicted instead of continuously being moved to the back of the queue.

Verified there on Bloodborne CUSA03173 (AMD RX 6600 8 GB): "Long play sessions (30m+) maintain stable
VRAM usage without running out of memory", and the 13,000-image mass-eviction stalls stopped.

Other reports of the same shape, all still open, all in that family:
`deadinside28/bloodborne_pc` #200 "VRAM grows steadily across area loads until
vk::Result=ErrorOutOfDeviceMemory (RX 6750 XT, 12 GB)" — its log shows `10138 of 5535 MiB (critical
8811): 30 images evicted … 0 old GPU-written tiled images kept (0 MiB)`, i.e. eviction running and
usage not moving: a *different* dominant holder; #32 "Crash every ~20 minutes (memory pressure)";
#377 "Crashing after 1 hour of gameplay"; #105 "rx 570 stop working after 15 minutes".

Upstream context: `shadps4-emu/shadPS4` #888 — `Failed allocating buffer … ErrorOutOfDeviceMemory` is
GPU OOM; the community workaround list for Bloodborne is the usual "lower resolution, no 60 FPS patch,
RAM-spike fix, 4 GB VRAM minimum" (r/shadps4 `1h8fsus`).

## 2. What our tree was missing

`TextureCache::GarbageCollectImages()` in this repo had the *identical* keep rule the PR describes:

```cpp
const bool download = image.SafeToDownload();
const bool tiled = image.info.IsTiled();
const bool drop = drop_old && tiled && download && aggresive && image.lru_touched_tick < minute_tick;
if (!drop) {
    if (tiled && download) {
        return keep(/*tiled_written_back=*/true);   // ← every depth target, forever
    }
    if (download && !pressured) {
        return keep(/*tiled_written_back=*/false);
    }
}
```

with no notion of depth at all, so every depth/stencil target and shadow map in `slot_images` was
touched and kept, exactly as the PR describes. Under pressure our collector therefore freed "1 image"
(Test #13) while sitting at 4504 of 4352 MiB.

## 3. What was changed (this round)

`gpu/shadps4/video_core/texture_cache/texture_cache.cpp`, inside the collector's `clean_up`, after the
pin gate and before the keep rules:

```cpp
if (depth_reclaim && image.info.props.is_depth && (pressured || aggresive)) {
    --num_deletions;
    ++gc_depth;
    evicted_any = true;
    FreeImage(image_id, "depth-reclaim");
    return false;
}
```

- `BB_GC_DEPTH_RECLAIM=0` switches it off for an A/B run.
- It is **pressure-gated** (`pressured || aggresive`), unlike the PR which reclaims depth
  unconditionally: a session with memory to spare keeps its depth buffers and its behaviour is
  unchanged, and the leak only matters once the collector has decided it needs room.
- Pinned images never reach it: `BbGpu::GcMayFreeImage` has already rejected bound/target/scene-proxy/
  video-out/collected ids, so a depth target the current draw or the upscaler holds is untouched.
- The pressure report gained a field: `… N pinned, M depth reclaimed`, and the counter resets with the
  others every 5 s.

Deliberately **not** taken from the PR: making *colour* render targets evictable under pressure. That
is what `BB_GC_DROP_OLD` already does when someone opts in, and the reason it is off by default here is
a measurement from this port — a dropped colour target the game still read produced a black screen with
sound on entering the Hunter's Dream on 6 GB cards (`handoff.md` §4.5, §11).

## 4. What this does not explain yet

`deadinside28` #200 shows usage stuck at 10138 MiB with eviction running and **nothing kept by the tiled
rule**, so there is a second holder that our numbers would have to name: buffers/heaps rather than
images, the pinned set, or the collector's own budget (`num_deletions`, `visited > 256`). Our next
`loader.log` separates those: the pressure line now reports evicted / written back / kept / dropped /
pinned / depth reclaimed, and the once-a-minute `Memory:` line reports VRAM, texture bytes, arena-backed
bytes, unused-for-a-minute images and of those the GPU-written tiled ones.

If `depth reclaimed` climbs and VRAM still parks on the mark, the culprit is not images and the next
suspect is the buffer/heap side (the dense arenas, the stream reserve, or the texture cache's own
budget) — that is the number the next log will carry.

Unverified on hardware, as always: CI says it compiles; a Series S/X run says it works.
