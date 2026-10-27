# Console log — build `.424`: the slot_vector stop is gone, the 5,120 MiB app limit is the wall

Console build `1.0.26300.424` (release `xbox-alpha-1.0.26300.0424`, target `9b09565`). Sent
2026-10-27, right after the crash and before relaunching.

| File | What it is |
|---|---|
| `loader.log.txt` | 814 lines: the whole session, the memory telemetry, the buffer headroom passes |
| `errors.log.txt` | the GPU/`stderr` side, ending in the `buffer.cpp:184` assertion |

## What it proves

- **The `slot_vector.h:225` stop is gone.** `grep -c "DEAD IMAGE"` = 0 and no `ValidateIndex` line in
  either file, and the run went through character creation into gameplay — §2g held.
- The new stop is an out-of-memory at the platform's app limit:

```
UWP buffer headroom pass 1..6: swap paged out 0 MiB (usage 5087/5120 MiB)
[uwp-vk] FATAL vmaCreateBuffer(16384 KiB, type=DeviceLocal) -> -2; Xbox AppMemory 5116/5120 MiB;
    VMA heap[0] block=1958 MiB alloc=1952 MiB usage=1958 MiB budget=4096 MiB
GPU [Debug] <Critical> buffer.cpp:184 Create: Assertion Failed! Failed allocating buffer with
    error ErrorOutOfDeviceMemory
```

## The numbers that matter

| Measurement | Value |
|---|---|
| App budget (`AppMemoryUsageLimit`) | 5,120 MiB — the same pool the GPU uses (`integrated, 5120 MiB VRAM`) |
| GC marks in this build | pressure 4,096 / critical 4,352 MiB (`limit-1024` / `limit-768`) |
| Previous build `.0345` marks, same console | pressure 4,352 / critical 4,672 — it hit the wall sooner |
| VMA device-local at the stop | 1,958 MiB of 5,120 — so ~3 GiB was guest-side |
| Guest **pinned** memory | 253 → 657 → 1,168 → 1,331 → 1,337 MiB (unpinned flat at 402–407) |
| Page-out before the failed 16 MiB buffer | 0 MiB in six passes |
| Dense GPU arenas at the stop | 832 MiB live, 52/176 DMA views |
| Frame rate | 55–58 FPS in the open areas, 28–40 FPS at the transitions |

Also seen once, four lines before the end: `UNHANDLED NATIVE FAULT: code=0xc0000005 … addr=0x0
rip=0x7ffd74acf5a0 module=unknown`. The run continued for several frames afterwards, so it is not
the reported crash — but it does not appear in any earlier log.
