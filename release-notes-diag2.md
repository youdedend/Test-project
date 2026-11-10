# diag-2 — the watchdog was killing the texture copies (publish notes)

Prepared by the research session, 2026-11-10. Tip `8cb181c` over the published diag-1 tip
`f531b31` (`xbox-alpha-1.0.26314.1903`). Build with the runbook (`NEW-CHAT-RUNBOOK.md`, v4).

## What the user reported

"i didnt have this problem with earler versions" — and they are right, the log proves it.

Both 26314 runs (`.1455` and `.1903`) died with the identical native fault:

    UNHANDLED NATIVE FAULT: code=0xc0000005 module=vulkan_dzn.dll+0x1fd79 ... addr=0x150
    UNHANDLED NATIVE FAULT: code=0xc0000005 module=vulkan_dzn.dll+0x1fd79 ... addr=0x148

`dzn_image_get_copy_loc()` reading `image->vk.base.device->physical` from a **stale dzn_image**
(device = 0xE0 / 0xD8 — garbage, not a device). The 26314.1903 log shows the whole sequence:

    1067  GPU: copy layer mismatch skip #1 ... addr=0x10d1d50000
    1068  Texture cache: free id=94 uid=94 by=other guest=0x10d1d50000 ... deferred=1
    ...
    1146  UWP watchdog: step release (arenas, staging, images) started ...
    1235  Texture cache: emergency reclaim freed 69 MiB of the 72 MiB asked for;
          seen=198 too recent=0 gpu-written=118 bound/target=1 no backing=5
    1428  UWP thread op: tid 5944 in 'recording Vulkan commands' for 2249 ms
    1466  UNHANDLED NATIVE FAULT: ... vulkan_dzn.dll+0x1fd79 ... addr=0x148

## The regression, dated

| build | watchdog frees texture images? |
|---|---|
| up to `xbox-alpha-1.0.26301.1156` (Oct 28) | **no** |
| `26302.0802` → `26314.1903` (now) | **yes** — `bbgpu_uwp_recovery_step` step 1 calls `TextureCache::EmergencyReclaim` |

`git log -S "bbgpu_uwp_recovery_step" -- gpu/shim/bbgpu.cpp` → `abb3d5c` "build 3: ... recovery
watchdog". Step 1 runs inside the watchdog's detached worker thread
(`xbox/platform/main.cpp:2941`), never on the render thread.

`EmergencyReclaim` takes the cache's `mutex`, but the renderer's index paths do not — `GetImage`
(`texture_cache.h:323`), the binding walks and the copy recording all read `slot_images` unlocked.
So the worker could erase an image the copy builder was still using. The file already applied the
opposite rule one line above, for staging: *"Staging's vectors are renderer-owned. Request its
normal idle trim there rather than mutating them from the watchdog worker."*

## The fix (this tip)

- **Recovery step 1 asks, the render thread performs.** New header-only policy
  `gpu/shadps4/video_core/renderer_vulkan/texture_cache_reclaim_request.h`; step 1 calls
  `RequestEmergencyReclaim(ask)`; `Rasterizer::OnSubmit()` calls `RunPendingReclaim()` at a frame
  boundary. Requests coalesce (largest budget wins), `0` means the 32 MiB default, and the freed
  total is reported. Reclaim still happens during a stall — just on the right thread.
- **diag-1's staging row fixed.** It printed `arena=0 MiB | dedicated=0 MiB` through a 2.4 GiB
  level-load burst because `GetStats()` never counted the ring blocks. Now prints
  `ring=<mib> MiB in <n> block(s), resident=<mib> MiB` too.
- **Fault report names the operation.** `bbgpu_thread_op_dump()` is now called from the
  native-fault filter, so the diag-1 tags (`copying image (texture transfer)`,
  `dense-arena merge copy`, `buffer-cache allocation`) appear next to the fault instead of only
  in the stall dump.
- **New CI test** `xbox/tests/texture_cache_reclaim_request_test.cpp` (registered in
  `.github/workflows/xbox.yml`): 6 checks on the hand-off.

## What is NOT claimed

That this is the only route to a stale image. It is the one difference between the builds that
crash and the ones that do not, and the only writer of image slots running outside the render
thread. Confirmation needs the hardware run below.

## The run to ask for

The same route: intro → character creation → confirm → world load. Expect in `loader.log.txt`:

- `Texture cache: render-thread reclaim of the watchdog's request freed N MiB ...` (instead of
  `UWP recovery: released ... images N MiB`, which now reads `images asked N MiB - deferred`)
- a `staging:` row with a non-zero `ring=` figure during the load
- no `UNHANDLED NATIVE FAULT ... vulkan_dzn.dll+0x1fd79`

Ask for `loader.log.txt` + `errors.log.txt` only.
