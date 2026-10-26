# Hardware Test #11 logs (build `xbox-alpha-1.0.26299.1047`)

Text logs uploaded from the Series S test of `xbox-alpha-1.0.26299.1047` (`a3d39fd`), kept verbatim
so the evidence behind the Test #12 fixes stays with the repository (the photos of the console are
not included).

| File | What it contains |
|---|---|
| `loader.log.txt` | Relaunch after the crash: stuck at `presents=0`, `1003/5120 MiB`, no fault. |
| `loader.prev.log.txt` | The crashing session (468 lines): prepare, patches, precompile, arenas, the fault. |
| `errors.prev.log.txt` | The fault report, five `SanitizeCopyLayers` coercions, the `precompile_inflight.txt` ENOENT. |
| `errors.log.txt` | Post-crash relaunch: only Vulkan extension/format warnings. |
| `dozen.log.txt`, `dozen.prev.log.txt` | Dozen startup lines for both runs. |

Key lines (verbatim):

```
Prepared boot image cached: ...\LocalState\Prepared (app_ver=01.00)
Compiled startup patches: game version 01.00 != 01.09; running unpatched 30 FPS (24 bytes)
Patches: 0 writes, 0 bytes applied
GPU: discarding stale driver.vkcache (172154 KiB, UUID/device mismatch)
UWP memory telemetry: 4135/5120 MiB (level=1, presents=1376)
Fault: read access to 0000020500987170 at bloodborne.exe+0x3003f0
UWP swap: out=248 in=20 disk=240 views=20 MiB; checks=1739 corrupt=0 window evictions=0
Runtime: save data 'SPRJ0005' mounted at /savedata0   (lines 229, 417, 419)
vk_runtime.cpp:60 SanitizeCopyLayers: Coercing copy source layers N and destination layers N+1
```

How each observation maps to a Test #12 step (see `handoff.md`, `## Hardware Test #12`):

- `app_ver=01.00` + `running unpatched 30 FPS` -> the missing patch set in this run (the 01.09
  prepare-time gate that briefly refused such dumps was rolled back; see `handoff.md`).
- `+0x3003f0` with `rcx-rdx = 0x2878A160` -> step 3, host page-table/`cached_pages` index fed by a
  guest address outside the 40-bit space (step 2 adds the linker map that resolves such frames).
- Two dense arenas live (`448 MiB` + `464 MiB`) right before the fault -> step 4, arena refcount.
- `driver.vkcache` discarded without the two identities -> step 6.
- `SPRJ0005` mounted three times -> step 7.
- Five layer coercions -> step 8.
