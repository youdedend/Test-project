# Console log — the run that confirmed skip-intro, and the crash it ended in

Build on the console: `1.0.26300.345` (release `xbox-alpha-1.0.26300.0345`, target `c63f0b7`).
Sent 2026-10-27, right after the crash and before relaunching, so these are the files of that run.

| File | What it is |
|---|---|
| `loader.log.txt` | launcher + `stdout` of the run: the skip-intro answer, the texture-cache frees, the memory-pressure report |
| `errors.log.txt` | the GPU/`stderr` side: ends with the `slot_vector.h:225` assertion and the STOP line |

`dozen.log.txt` was sent too and held only the two `dzn is not a conformant Vulkan implementation`
warnings — nothing about this run, so it is not kept here.

## What the run proved

- The skip-intro question works end to end (§2f): the launcher asked, wrote `skip_intro=1` into
  `LocalState\bbport.ini`, printed `Launcher panel: intro skipped`, and the intro did not play.
- The crash after name entry is unchanged, and this log pins its mechanism for the first time:

```
Texture cache: memory pressure, 4726 of 4096 MiB (critical 4352): 9 images evicted ...
Texture cache: free id=58 uid=58 by=eviction guest=0x1066ffe000 size=1048576 ... deferred=1
Texture cache: erase id=58 (freed by=eviction)
Runtime: ImeDialog text: Hunter
Runtime: save data 'SPRJ0005' mounted at /savedata0 (existing)
slot_vector.h:225 ValidateIndex: Assertion Failed!
SlotVector invalid or freed slot index=58 capacity=2978
```

  `capacity=2978` is one 2 MiB chunk of `slot_images` (`sizeof(Image)` ≈ 704 B), so the stale id is
  an `ImageId`, and `58` is exactly the image the collector had just erased. `grep -c "DEAD IMAGE"`
  is **0** in both files: the id was not used through `GetImage`/`GetImageView` but through a direct
  `slot_images[...]`, which is what §2g instruments and fixes.

## Numbers worth keeping

| Measurement | Value |
|---|---|
| Frame rate before the crash | 27.3 FPS, slowest frame 96.0 ms |
| Memory high-water | `4333/5120 MiB (level=1, presents=870)`, back to `3237` after the GC |
| Images evicted by that pressure pass | 9 (512 B – 1 MiB, all `tiled=1`), `0 written back`, `0 pinned`, `0 dropped`, `0 depth reclaimed` |
| UWP swap at the end | `unpinned private=187 pinned=266 MiB` |
