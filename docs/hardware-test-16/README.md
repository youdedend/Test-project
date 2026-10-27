# Console log — build `.504`: same allocation wall, and why it *looks* like 4,600 MiB

Console build `1.0.26300.504` (release `xbox-alpha-1.0.26300.0504`, target `d37adf7`). Sent
2026-10-27, right after the crash and before relaunching. This is the first build that prints
`guest pinned=`.

| File | What it is |
|---|---|
| `loader.log.txt` | 810 lines: the session, the 10 telemetry samples, the arena burst, the headroom passes |
| `errors.log.txt` | GPU/`stderr` side, ending in the `buffer.cpp` assertion |

## What it proves

- Still **no** `slot_vector.h` stop and no `DEAD IMAGE` — the freed-slot fix (§2g) is stable.
- The stop is again an allocation failure at the platform limit:

```
UWP memory telemetry: 4628/5120 MiB (level=2, presents=801, guest pinned=1350 MiB)
UWP buffer headroom pass 1: swap paged out 64 MiB (usage 4948/5120 MiB)
... 4936, 4958 (64 MiB each), then 4977, 5028, 5045, 5077, 5110 (0 MiB each)
[uwp-vk] FATAL vmaCreateBuffer(16384 KiB, type=HostUncached) -> -2;
    Xbox AppMemory 5108/5120 MiB (guest pinned=1351 MiB);
    VMA heap[0] block=2157 MiB alloc=2151 MiB usage=2157 MiB budget=4096 MiB
```

## The numbers that matter

| Measurement | Value |
|---|---|
| Last telemetry printed before the crash | 4,628 MiB (presents=801) — but the crash line says **5,108 MiB** |
| Last texture-cache report | `4612 of 4096 MiB (critical 4352, guest pinned 1351)` — `4096` is the pressure mark, not the ceiling |
| Telemetry lines in the whole session | **10** (`sample < 24` cap, 5 s cadence) — the tail is unlogged |
| Dense GPU arenas during the last burst | `live 496 → 912 MiB` (+416 MiB), `31 → 57` DMA views |
| Guest **pinned** memory | 0 at boot → 167 → 465 → 1,013 → 1,216 → 1,350 → 1,351 MiB |
| Guest unpinned private | 401 MiB, flat |
| VMA device-local at the stop | 2,157 MiB used of a 4,096 MiB driver budget |
| Page-out in the five passes before the failed 16 MiB buffer | **0 MiB** |
| Frame rate | 17–29 FPS (heavier area than the `.424` run) |
