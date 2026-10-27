# Bloodborne Xbox Series X|S UWP Port — Engineering Handoff

**Read this first.** This document is the complete record of the port: what it is, what has been
built, what broke, what was changed to fix it, how it is built and tested, and what is still
unproven. Sections 1–9 are the current, authoritative state. Section 10 is an **archival appendix**
kept word for word from earlier rounds (it explains how the current state was reached; its branch
names, PR numbers and "current" claims are historical and superseded).

Last updated: after commit `03b74ac` (CI run `40432005224`, green). Console-tested: **no**.

---

## 1. What this project is

Build and ship a playable Xbox Series X|S Developer Mode UWP port of `bloodborne_pc`
(`Supermedo/bloodborne_pc`, `windows` baseline `5adea35dbd38`) in `youdedend/Test-project`, compiled
and signed end to end inside GitHub Actions, so a user can sideload the `.appx` on a console, point
it at their own decrypted PS4 Bloodborne `v01.09` dump (`eboot.bin`, `sce_module`, `sce_sys`,
`dvdroot_ps4`) and play natively on the console.

Standing goals and constraints:

- **Native CPU + HLE + GPU.** PS4 x86-64 ELF loader (`src/probe.c`) and HLE kernel/libc/thread/pad/
  audio runtime (`src/runtime*.c`) compiled with `clang-cl` (`sysv_abi` bridge) beside the shadPS4
  Liverpool GPU emulator (`gpu/shadps4/**`); Vulkan on Xbox through packaged Mesa Dozen
  (`vulkan_dzn.dll`, `Cosmo/mesa_ai_slop_version` @ `a76cd3ca…`, MSVC 14.44 — never 19.51, which
  ICEs), DXC `v1.9.2609` (`dxil.dll`), and a native D3D12 flip-model present bridge
  (`xbox/graphics/present_bridge.cpp`).
- **In-app native C++ dump preparation**, no Python on the console
  (`xbox/platform/prepare_game.cpp`, `prepared-format.txt` = `2`), enforcing `APP_VER = 01.09`
  whenever `patches/redzone.json` targets 01.09 offsets.
- **One 5,120 MiB app-memory limit** for guest memory, images, buffers, Dozen/D3D12 heaps and the
  pipeline cache — hence all of the eviction work in §4.
- **Ahead-of-gameplay shader/pipeline precompilation** from `.shaderbnd.dcx` (BND4 + DCX) with
  crash recovery via disk breadcrumbs (`gpu/shim/bb_precompile.cpp`).
- **Complete console UX:** `Windows.Gaming.Input` gamepad + rumble, `L3 + R3` settings overlay,
  virtual keyboard for Hunter Name, XAudio2, LibAtrac9/FFmpeg AvPlayer, dump discovery from
  `D:\DevelopmentFiles\Bloodborne`, `LocalState\game` or USB.
- **Strict UWP and legal:** zero `runFullTrust` / `broadFileSystemAccess`; every PE audited by
  `xbox/scripts/check-imports.ps1` against `WindowsApp.lib`, the Store CRT or packaged DLLs; no Sony
  binaries, keys or ISOs in Git; test signing uses the intentionally public
  `xbox/signing/bloodborne_xbox-signing.pfx`.

---

## 2. Where things stand right now

| Item | Value |
|---|---|
| Repo / branch | `youdedend/Test-project` — `arena/8ac58bdb-test-project` (session branch; the platform fixes this name, it cannot be renamed) |
| HEAD | `9b165b4` — *gpu(texture_cache): report direct slot indexes, and re-check memos* (release target) |
| `main` | `36e388d1123b7a46272043577826c86297d81712` — untouched by this work |
| Last green CI | run `40446937105` — [actions/runs/40446937105](https://github.com/youdedend/Test-project/actions/runs/40446937105), both jobs green, all tests green, pre-release published |
| Package built by it | `BloodborneXbox-1.0.26300.405` — contains `BloodborneLauncher_1.0.26300.405_x64.appx` + `Dependencies/Microsoft.VCLibs.x64.14.00.appx` |
| Version shown by that package | **`1.0.26300.405`** (app list / "Manage app" / `.appx` filename — *not* printed in `loader.log`) |
| Published pre-release | **`xbox-alpha-1.0.26300.0405`** — [releases/tag/xbox-alpha-1.0.26300.0405](https://github.com/youdedend/Test-project/releases/tag/xbox-alpha-1.0.26300.0405), target `9b165b4` (run `40446937105`, both jobs green, publish step ran): **every direct `slot_images` index reports the stale id, and the three cross-frame memos re-check liveness** (§2g). Previous: `.0345` (skip-intro question + `[dump]` preserved, §2f), `.0328` (depth reclaim, §2e), `.0300` (precompile isolation, §2c) |
| Console validation of this build | **not installed yet** — the console last ran `1.0.26300.211`; `.0405` (skip-intro question, depth reclaim, stale-id report + memo fix) is published and waiting to be added through Device Portal |
| Open blocker | the post-character-creation crash (see §4.5) is *addressed but unproven* — the freed id is now either avoided (memos) or named (report) |
| CI for the precompile isolation (§2c) | run `40439891299` on `0234b1a` (both jobs green) built `1.0.26300.246`; it was then published as `1.0.26300.300` by run `40440888038` on `7535ba2` |

Compile-state caveat: everything in §4 has been compiled **only** by CI (clang-cl). Two compile
errors were caught and fixed that way (§7). No local GPU build is possible in the agent sandbox (see
§8), and no compiler warnings from the changed files have been read (the workflow does not annotate
warnings and the log endpoints are unreachable from the sandbox).

---

## 2b. Test #13 logs (2026-10-27, after `1.0.26300.211` was published) — the OLD build ran

The six log files supplied after the release are from the **pre-fix binary**, not from
`1.0.26300.129` / `1.0.26300.211`. **Confirmed on the console**: the Games and apps list shows
`BloodborneLauncher_1.0.26300.34_x64__5c4hjcx6a8gg4`, i.e. the `.34` package built from
`4d1797d`, so the newer `.appx` never registered (the upgrade attempt either was never made or
failed registration). Proof, all from `loader.log.txt` / `errors.log.txt`:

| Evidence | Old build | This run | New build would print |
|---|---|---|---|
| Pressure-line format | no pinned field | `… 49 old GPU-written tiled images kept (0 MiB), 0 dropped` | `… , N pinned` |
| Memory marks | −768 / −448 | `4504 of 4352 MiB (critical 4672)` | `of 4096 MiB (critical 4352)` |
| Per-free trace | absent | 0 occurrences of `Texture cache: free id=` | one per freed image |
| Erase trace | absent | 0 occurrences of `Texture cache: erase id=` | one per erased slot |
| Dead-id report | absent | 0 occurrences of `DEAD IMAGE` | printed before the assertion |

So both runs (this one and the `.prev` pair) are the same old binary: identical fingerprint
`slot_vector.h:225 ValidateIndex`, `SlotVector invalid or freed slot index=132 capacity=2978`.

What the old-build logs do establish:

- **Crash position:** right after the character-creator threads start (`CSChrThread0..5`,
  `CSChrBackInitialize1`, `CSClothThread0..5`) — i.e. in the character creator, as reported.
- **Memory at the crash:** `AppMemoryUsage` 4504 MiB of the 5120 MiB limit (616 MiB headroom),
  `AppMemoryUsageLevel` still `0`. Telemetry: 84 → 1148 → 1027 → 1974 → 2299 → 2292 → 2298 MiB
  (last sample at `presents=1010`); the jump to 4504 MiB happens after telemetry stops printing
  (it only prints for the first 24 samples).
- **Exactly one image was evicted by that pressure report** (`1 images evicted`), and the assertion
  follows within the same frame — consistent with the diagnosed cause, but the old binary cannot say
  *which* image, because it has no free trace. This is precisely what the new build adds.
- **Configuration in use:** split dump (`base=01.00` + `patch=01.09`, `effective=01.09`,
  `patches=required`, 436 overlay paths, digest `ee93feb5f771`), `Compiled startup patches: FPS=60,
  scene=1920x1080, UI=1920x1080, writes=196`, `Scene resolution: raster 640x360, half-resolution
  targets 320x180, UI 1920x1080 (live)`, `FSR1 and RCAS disabled`, 30→60 FPS patch. 56–59 FPS,
  slowest frame 116 ms.
- **No precompiled pipeline cache:** `driver.vkcache` is missing under
  `LocalState\user\cache\CUSA03173\`, so shaders compile at runtime (that is the 116 ms frame, not
  a crash factor). Running **Precompile shaders** once from the launcher removes it.
- **Only the previous run** logged `liverpool.cpp:70 NextPacket: packet length exceeds remaining
  submission size. Packet dword count=2, remaining submission dwords=1` — a truncated PM4 submission.
  It did **not** appear in this run. If it reappears, it is a separate guest-command-stream bug worth
  its own investigation.

Next action: install **`1.0.26300.211`** from
`https://github.com/youdedend/Test-project/releases/tag/xbox-alpha-1.0.26300.0211`, confirm the
version in "Manage app", and repeat. If the version shows `.211` **and** `loader.log` still has no
`Texture cache: free id=` lines, the new code is not in the binary — report that immediately.

---

## 2c. Precompile crash at item `1025 / 10256` (2026-10-27) — crash isolation added

The log batch supplied before this round (`loader.log.txt` 102 lines + `errors.log.txt`) never reached
gameplay: telemetry stops at `1077/5120 MiB (presents=0)`. The run discards a stale `driver.vkcache`
(172,154 KiB, "pipeline cache UUID changed"), extracts 8,207 CS/PS/VS programs and 2,048 linked pairs,
imports the portable cache, warms 65/65 pipelines, then flushes `driver.vkcache` at the
`i % 256 == 255` boundary (5403 → 14195 → 23429 → 31339 KiB) and dies on the item after 1024. Last
GPU line: `control_flow_graph.cpp:407 RemoveUnreachableBlocks: Removing unreachable block begin=0x27c`.
No `pinned` marker and `Scene resolution: raster 1280x720`, so this is the precompile run, before any
texture pressure — it is **not** the texture-cache crash.

Why nothing was logged: the recompiler's `abort()`/fastfail path in the static UCRT bypasses the
loader's VEH, so only `precompile_inflight.txt` survived, and it recorded the item but not what the
item was doing — a poisoned shader therefore cost one relaunch per shader, blind.

Changes, all in `gpu/shim/bb_precompile.cpp` (202 insertions, 8 deletions):

| What | Where |
|---|---|
| `enum class CompilePhase { Idle, Params, Translate, Emit, Validate, Pipeline }`, `PhaseName` | `bb_precompile.cpp:80-108` |
| `g_compile_phase`, `g_active_breadcrumb` (forward-declared `InflightBreadcrumb`) | `:99-102` |
| `ScopedActiveDiag` dtor resets the phase to `Idle` | `:120` |
| Breadcrumb record is now 5 fields (`hash partner kind idx phase`), kept in RAM and rewritten by `SetPhase` | `:453-521` |
| `SetCompilePhase()` — one atomic store + a small file rewrite per phase; **clears `runtime_fault_recover` on `Pipeline`** so Mesa's own SEH is not hijacked | `:532-549` |
| `ConsumeInflightCrashBreadcrumb` prints kind + phase (e.g. `died mid-compile on item 1025 (kind=1 phase=2 translate …)`) | `:405-425` |
| Phase markers in all three standalone branches (CS/PS/VS) and both linked-pair branches | `:1692-1971` |
| Phase markers in the aux-GS warm | `:2035-2040` |
| `sigsetjmp`/`runtime_fault_recover` isolation around `CompileSingleExtractedInner`, `CompileLinkedPairInner` and each aux-GS variant → `OnCompileFault()` prints, counts, quarantines and continues | `:1989-2010`, `:2064-2125` |
| `OnPrecompileTerminate()` installed with `std::set_terminate` for the duration of the loops: an unhandled exception still names item/phase/hash | `:2055-2066`, `:2433-2443` |
| Summary line `N shader(s) faulted the compiler and were quarantined` | `:2539-2545` |

Behaviour now, in order of what the crash actually is:

1. **Access violation inside the recompiler** → recovered at the `sigsetjmp` point, shader quarantined,
   run continues in the same launch.
2. **Anything that still kills the process** (`abort`, stack overflow, driver thread) → terminate
   handler / crash report names the item and phase, and the next launch quarantines that hash from
   the breadcrumb and resumes — one relaunch for that shader instead of one per shader.

Not recovered on purpose: faults inside the driver call (`Pipeline` phase) hand the recovery point
back before entering Mesa, and a `longjmp` out of a faulting recompiler frame skips destructors — so a
fault leaks that shader's IR pools. Both are bounded and deliberate.

**Unproven on hardware** — as with everything in this document, CI (clang-cl) says it builds: run
`40439891299` on `0234b1a`, both jobs green, UWP platform tests green, package
`BloodborneXbox-1.0.26300.246` built as a run artifact (no release was published for it).

---

## 2d. Community research on the precompile crash (2026-10-27)

Full report: [`docs/PRECOMPILE_CRASH_RESEARCH.md`](docs/PRECOMPILE_CRASH_RESEARCH.md). Summary:

- **Nobody reports this crash.** No shadPS4 issue/PR mentions `RemoveUnreachableBlocks`,
  `GotoPass`/`kMaxGotoTransforms` or a precompile step (upstream has no precompile feature).
  GitHub issue search plus web search turned up nothing for the same symptom.
- **The sibling ports precompile the same game without dying**, which is the useful finding:
  `Jgamrot/bloodborne_pc` builds a *catalog* of shaders the game is known to use;
  `visy/bloodborne_pc_windows_port` compiles only the *recorded* pipeline cache and states
  "shaders the game has never used on any machine cannot be prepared up front: a pipeline depends on
  draw state the game only sets while running"; `Awakenchan/LanternEngine` ships ~510 pipelines.
  We sweep 10,256 synthetic archive items instead — one to two orders of magnitude more than the game
  uses.
- **The habit that shows up in every real root-cause**: name the shader and fail open
  (`axylpup/shadPS4` #25: "no longer closes when the driver refuses one of the faster shader
  versions… the log names the shader so I can look at it"), and replay the offending shader offline
  (`boykopovar/AnyPS5` #9702 found a 177 s structurization that way). §2c's quarantine is the first
  half; a source dump/journal would be the second.
- **Rules out** "cherry-pick an upstream fix": upstream still has the same
  `RemoveUnreachableBlocks` shape (ours is stricter), and this tree's frontend carries hardening
  upstream does not have (goto-transform cap 65536, tree guards, `VisitExpr` depth limit 512).

Three routes, in the report: **A** precompile only what the game uses (catalog, like the siblings) —
makes the crash stop; **B** keep the sweep and make one bad shader cost nothing (time budget, resume,
journal, retry list) — extends §2c; **C** journal each shader and replay it offline — diagnosis, the
prerequisite for fixing the real frontend bug.

---

## 2e. The high-GiB crash — community fix applied (2026-10-27)

Full report: [`docs/MEMORY_CRASH_RESEARCH.md`](docs/MEMORY_CRASH_RESEARCH.md). Short version:

- **A sibling port hit this with our exact log lines.** `Mrsuss60/bloodborne_pc_windows_port` #75
  ("game crash due to vram leak") shows `Texture cache: memory pressure, 3889 of 3729 MiB (critical
  3936) … 118 images evicted` → `vk::Result=ErrorOutOfDeviceMemory` → `STOP: GPU library assertion
  failed` → exit 23, reported on 4 GB and 8 GB cards after 15–30 minutes. #69 is the same growth per
  zone load (200–500 MB per transition). Both were closed by its **PR #89**, which names the cause:
  GPU-written **tiled depth/stencil targets have `SafeToDownload() == true`, cannot be written back,
  and were touched and kept alive forever — "locking gigabytes of VRAM"**, and under pressure the
  collector moved tiled images to the back of the queue instead of evicting them.
- **Our tree had the identical keep rule and no notion of depth**, which is why Test #13 evicted
  "1 image" while parked at 4504 of 4352 MiB.
- **Change** (`texture_cache.cpp`, in the collector after the pin gate): under pressure, a
  non-pinned `is_depth` image is freed at once (`FreeImage(id, "depth-reclaim")`), counted in the
  pressure report as `N depth reclaimed`; `BB_GC_DEPTH_RECLAIM=0` disables it for an A/B run. Unlike
  the sibling PR it is **pressure-gated**, so normal sessions keep their depth buffers unchanged.
- **Not taken:** making colour render targets evictable under pressure — that is `BB_GC_DROP_OLD`
  (opt-in here for a measured reason: a dropped colour target produced a black screen with sound on
  the Hunter's Dream on 6 GB cards).
- Still open: `deadinside28/bloodborne_pc` #200 shows usage stuck at 10138 MiB *with* eviction running
  and nothing kept by the tiled rule, i.e. a second holder (buffers/heaps or pins). The next
  `loader.log` separates them: pressure line now carries evicted / written back / kept / dropped /
  pinned / depth reclaimed.

---

## 2f. "Skip the intro" option (2026-10-27)

The PC port has `skip_intro` (its `scripts/patches.py` turns it into the "Skip Intro" patch), and
`patches/Bloodborne.xml` here already carried that patch — but on the console there was no way to
turn it on *before* the intro plays: the only switch was the in-game overlay's checkbox, and the
overlay cannot be opened until the game is running, by which time the intro is over.

What was added:

| What | Where |
|---|---|
| Ask once, before the first start that applies patches: "Skip the intro?" (`Play the intro` / `Skip the intro`, dismissal = play) | `xbox/platform/main.cpp:887-910` (`ask_intro_once`) |
| Called from the launcher before the dump panel, so the panel's `Intro: …` line shows the answer; skipped in headless `run.txt`/self-test starts | `main.cpp:928-933` |
| Panel text now carries the current state and says where to change it later | `main.cpp:949-956` |
| `skip_intro` written as a bare key (the layout `scripts/patches.py` and the Windows launcher use), preserving every other line | `main.cpp:704-717` (`set_ini_top_level`), `xbox/platform/ini_file.h:199-260` (`BbIni::SetTopLevel`) |
| Default ini no longer pre-sets `skip_intro`: its *absence* is what makes a fresh install ask once | `main.cpp:638-660` |
| Tests for the new ini writer (5 cases: insert, replace, duplicate collapse, same value, empty file) | `xbox/tests/ini_file_test.cpp:150-207`, run by CI's `ini_file` test binary |
| The in-game path made to work end to end: `BbSettings::Save()` rewrote `bbport.ini` from scratch and **dropped the launcher's `[dump]` section**, so toggling "Skip intro logos and movie" in the overlay also forgot the patch folder. `Save()` now carries every `[section] …` block over | `gpu/shim/bbport_settings.cpp:353-380`, `:382`, `:429-431` |

Why a separate dialog and not a fourth launcher button: `Windows.UI.Popups.MessageDialog` accepts at
most **three** commands on desktop (a fourth throws `E_INVALIDARG`), and every panel here already has
two or three. `ask_player` also gained an explicit `cancel_index` so a dismissal cannot be mistaken
for a choice (`main.cpp:182-199`).

Verification: `xbox/tests/ini_file_test.cpp` compiled and run locally with g++ — all checks pass
(`bbport.ini round-trip: all checks passed`, 5 new cases among them); `tests/test_patches.py`
15 passed / 6 skipped; `gpu/shim/bbport_settings.cpp` passes `g++ -fsyntax-only`. `main.cpp` needs
WinRT and can only be compiled by CI.

Not verified on hardware — as usual, CI says it builds; a Series S/X launch says it works. Expected
log line after the answer: `Launcher panel: intro skipped`, and with patches applied the intro is
gone on the next start.

---

## 2g. `slot_vector.h:225` after name entry — the stale-id site, and the memo fix (2026-10-27)

The run that confirmed skip-intro ended in the old character-creation stop, and this time the log
says exactly what happened (the two files are kept in `docs/hardware-test-14/`):

```
Texture cache: memory pressure, 4726 of 4096 MiB (critical 4352): 9 images evicted ...
Texture cache: free id=58 uid=58 by=eviction guest=0x1066ffe000 size=1048576 ... deferred=1
Texture cache: erase id=58 (freed by=eviction)          <- the deferred EraseImage, slot 58 dies
Runtime: ImeDialog text: Hunter
Runtime: save data 'SPRJ0005' mounted at /savedata0 (existing)
GPU [Debug] <Critical> slot_vector.h:225 ValidateIndex: Assertion Failed!
SlotVector invalid or freed slot index=58 capacity=2978
```

`capacity=2978` is one 2 MiB chunk of `slot_images` (sizeof(Image) ≈ 704 B), so it is an **ImageId**
and not a view, and `58` is the image the collector had just erased.

**Why the existing DEAD IMAGE instrumentation stayed silent:** `ReportDeadImage` only runs from
`GetImage`/`GetImageView`. `grep -c "DEAD IMAGE"` is 0 in `loader.log.txt`, `errors.log.txt` and
both `.prev` files, so the stale id was never passed to those two. It was indexed **directly**:
`TextureCache` has ~40 `slot_images[...]` uses of its own (`MarkFound`, `IsUpToDate`, `UpdateImage`,
`FindTexture`, `FindRenderTarget`, `FindDepthTarget`, `DownloadImageMemory`, the collector, …), and
none of them reported anything. `slot_vector.h` is not touched — the assertion stays exactly as it
is.

What changed:

| What | Where |
|---|---|
| `IndexImage` / `IndexImageView`: the direct index wrapped so a freed slot prints the same report (id, caller's `file:line`, last frees) before the unmodified assertion. One `is_allocated` test per index, under `BB_UWP` | `texture_cache.h:245-276` |
| Every in-class direct `slot_images[...]` now goes through it (`MarkFound`, `IsUpToDate`, `UpdateImage`, `ForEachImageInRegion`, `GetAssociatedDepth`-adjacent sites) | `texture_cache.h:111, 157, 174, 186, 384, 406` |
| Same for all 37 sites in the cache's translation unit, plus the one unguarded `slot_image_views[...]` (the view memo's `ids[last]`) | `texture_cache.cpp:143, 257, 309, 312, 332, 376, 547, 562, 563, 596, 609, 629, 657, 669, 686, 734, 748, 772, 780, 806, 836, 840, 843, 960, 975, 1001, 1027, 1042, 1058, 1072, 1090, 1127, 1298, 1519, 1545, 1565, 1626, 1656, 1721` |
| **The fix candidate itself:** the three memoized lookups that keep an `ImageId` between frames and then index it directly now also require the slot to still be allocated, and re-resolve when it is not. `MarkFound` is what they call, and it is a direct index — a memo hit on a slot freed since stopped the GPU with no hint of the path | `vk_rasterizer.cpp:190` (`last_targets`), `:214` (`target_memo`), `:2376` (`image_desc_cache`/`desc_entry`) |

Why the memos are the suspect: they are the only places that reach a direct `slot_images[...]`
**without** going through `GetImage` first. Every other site that uses a stored id either re-checks
with `is_allocated`/uid (`GetAssociatedDepth`, `TryGetImage`, `TryGetImageTouched`, the upscaler,
camera motion, scene resolution) or is reached after a guarded `GetImage` in the same iteration, so
it would have printed `DEAD IMAGE` — and nothing did.

Note what this is and is not: the generation stamp the memos already check (`RegistryGeneration()`)
changes on every register *and* unregister, so a **finished** free can never leave a memo looking
valid. What the stamp cannot see is the window inside one lookup, and above all it does not by
itself make the id safe to index — the aliveness test does, and it is one bitset read on a branch
that is already taken only on a memo hit.

Verification: `xbox/tests/texture_cache_policy_test.cpp` rebuilt and run with g++ (host) — all
checks pass; `tests/test_patches.py` unchanged and green. `texture_cache.cpp`/`.h` and
`vk_rasterizer.cpp` are Vulkan/MSVC-only here (no local Vulkan or boost headers), so CI is the
compile check. **Not verified on hardware.** If it is not this, the next run prints
`Texture cache: DEAD IMAGE id=… indexed by index slot_images at <file>:<line>` immediately before the
same assertion, which names the site outright.

---

## 3. Code map (the files that matter)

**Build / CI / packaging**
- `.github/workflows/xbox.yml` — two jobs. `dozen` builds `vulkan_dzn.dll` (MSVC 14.44, cached);
  `xbox` fetches DXC + third-party deps, builds `bloodborne.exe` with clang-cl, runs the six unit
  suites, audits imports, packs and signs the `.appx`, uploads artifacts, and publishes a
  pre-release **only** when `[release]` is in the head commit message or a `workflow_dispatch` run
  asks for it (`release` defaults to `true`, so dispatch with `-f release=false` to build only).
- `xbox/CMakeLists.txt` — UWP target (`WindowsStore`, `10.0.22621.0`, `/APPCONTAINER`,
  `/HIGHENTROPYVA:NO`, `/Oy-`, `/STACK:8388608,65536`).
- `xbox/AppxManifest.xml`, `xbox/prepared-format.txt` (format `2`), `xbox/signing/*`.
- `xbox/scripts/{fetch-deps.py,boost-headers.py,build-deps.ps1,build.ps1,check-imports.ps1,package.ps1,assets.ps1,symbolicate.py,crash-rvas.txt}`.

**Platform / launcher / memory / HLE**
- `xbox/platform/main.cpp` — UWP lifecycle, launcher UI, `FutureAccessList` USB tokens,
  `bbport.ini` preset sync, last-chance crash breadcrumbs, `STOP:` dialog.
- `xbox/platform/prepare_game.{cpp,h}` — SELF/ELF unpacker, PRX linker, red-zone/`sysv_abi` patcher,
  `Bloodborne.xml` patch applier, 01.09 gate, split-dump detection.
- `xbox/platform/guest_memory.inc`, `guest_swap.inc` — sub-1 TiB reservation, 4 KiB GPU
  write-tracking, `WC_GARLIC`-only sparse swap (`LocalState\garlic_swap.bin`).
- `xbox/platform/{input.cpp,audio.cpp,runtime_compat.c,desktop_shims.cpp}`, `src/probe.c`,
  `src/runtime_*.c`.

**GPU**
- `gpu/shadps4/video_core/texture_cache/` — `texture_cache.{h,cpp}` (collector, free trace,
  dead-image report), **new** `texture_cache_policy.h` (pin rules).
- `gpu/shadps4/video_core/renderer_vulkan/` — `vk_rasterizer.{h,cpp}` (binding walk, held ids,
  `OnSubmit`), `vk_temporal_upscaler.{h,cpp}` (upscaler + FSR/DLSS + frame gen), `vk_camera_motion.*`,
  `vk_instance.cpp` (5,120 MiB UMA + `VK_EXT_memory_budget`), `vk_draw_prep.*`, `vk_pipeline_cache.cpp`,
  `vk_pipeline_serialization.cpp`, `vk_graphics_pipeline.cpp`.
- `gpu/shadps4/video_core/{cache_storage.cpp,amdgpu/regs_shader.h}`,
  `gpu/shadps4/shader_recompiler/**` (CFG/SCF/SPIR-V fixes), `gpu/shim/bb_precompile.cpp`.
- `xbox/graphics/{present_bridge.cpp,swapchain.cpp,window.cpp,dense_buffers.cpp,descriptor_fallback.cpp}`.

**Tests**
- `xbox/tests/` — `sfo_version`, `overlay_index`, `ini_file`, `dense_arena_lifetime`,
  **new** `texture_cache_policy`, `dump_layout` (links the real `prepare_game.cpp`).
- `uwp/tests/test_uwp_port_core.cpp` — host C++20 suite for UWP platform/input/storage/D3D12 caches.
- `tests/` — desktop regression suites (Python + `test_content.c`).

---

## 4. Work log — everything done on this port, in order

Each round: what was wrong → what was changed → how it was verified.

### 4.1 CI, strict UWP packaging, console launcher (`26303d9`..`5e37667`)
Built `.github/workflows/xbox.yml`: Dozen (MSVC 14.44), DXC fetch, clang-cl build, AppContainer PE
import audit, `.appx` sign/pack, `[release]`-gated pre-releases. Replaced UWP-blocked file type
associations in the manifest with PS4 archive extensions (`.dcx`, `.bhd`, `.bdt`, `.bnd`, …) and
persisted USB folder permissions via `FutureAccessList`.

### 4.2 Native C++ dump preparer and sub-1 TiB guest runtime (`2f694dc`..`05d2d04`)
`prepare_game.cpp` (format 2) with `APP_VER = 01.09` enforcement; every guest pointer, thread stack,
TLS block, GOT entry and AvPlayer buffer kept below `1 TiB`; `GuestMutex` layout fixed; `clang-cl`
atomics; swap restricted to `WC_GARLIC` with fault-time pinning (`corrupt=0`).

### 4.3 5,120 MiB UMA, swapchain, dense arenas, stack flags (`19a8772`, `7197c87`, `9b74bb3`)
Dozen's 1,280 MiB Series S cap overridden to expose the 5,120 MiB Game Mode budget; `NUM_DMA_BUFFERS
= DmaViewCount = 176`; `Swapchain::frame_index = 0` for the single-image bridge; superseded dense
`VkBuffer`s retired through `scheduler.DeferOperation` with a pending-bytes counter; `/STACK` changed
from 16 MiB commit to `8 MiB reserve / 64 KiB commit`, reclaiming ~1.25 GiB of eagerly committed RAM
across ~82 OS threads.

### 4.4 Shader precompilation and compiler fixes (`360f2ff`..`9b74bb3`)
Offline `.shaderbnd.dcx` scanner + PSO builder with `precompile_inflight.txt` /
`precompile_quarantine_v2.txt` crash recovery; UUID-validated `driver.vkcache`; fixed the
`get_future()` double-wait, W^X SRT walker arena, `Id{0}`/`PerVertex`/`F32[0]` emission, DFS
reachability in `RemoveUnreachableBlocks`, `OpReturn` instead of `OpUnreachable` on `BB_UWP`, and the
`GotoPass::MoveOutwardIf` sentinel corruption (shader `i = 1024`).

### 4.5 Post-character-creation crash — round 1 (AvPlayer) and round 2 (texture cache)
**Round 1** (`xbox-alpha-1.0.26299.2359` logs): community reports and logs pointed at the audio/
cutscene path; fixed in `gpu/shadps4/core/libraries/avplayer/` (frame-buffer allocation assert).

**Round 2** (`xbox-alpha-1.0.26300.34` logs) — the live blocker:

```
Texture cache: memory pressure, 4493 of 4352 MiB (critical 4672): 1 images evicted
slot_vector.h:225 ValidateIndex: Assertion Failed!
SlotVector invalid or freed slot index=132 capacity=2978
STOP: GPU library assertion failed
```

Two independent defects, both from `Common::SlotId` being a bare `u32` index with **no generation**:

1. **The collector freed images that were still held.** `GarbageCollectImages`' `clean_up` decided on
   `SafeToDownload()` / `IsTiled()` / LRU age alone and never asked whether the image was in use,
   while `ReclaimMemoryFor` had always skipped `scene_proxy`, `usage.vo_surface`, `binding.is_bound`
   and `binding.is_target`. Under pressure a bound render target or the video-out surface the
   presenter reads could be evicted, and the rasterizer/presenter/upscaler then indexed the freed slot.
2. **Every stored `ImageId` was re-read with `GetImage()`**, which asserts on a dead slot and
   silently aliases a reused one. Holders: the upscaler (`scene_color`, `ldr_target`, `ui_color`,
   `ui_depth`), `CameraMotion::depth_id`, `Rasterizer::bound_images`.

Fixes now in the tree:

- **Pin policy** in the new `texture_cache_policy.h` (`BbGpu::ImagePinState`, `ImagePinned`,
  `GcMayFreeImage`), applied by `clean_up`; pinned candidates cost no deletion and get their LRU entry
  touched, so a run of unevictable images cannot stall or exhaust the bounded scan.
- **Ids the draw already resolved but has not bound yet** are held: `Rasterizer::CollectImage()` at
  all five collection sites → `TextureCache::HoldCollectedImage`, cleared in `ResetBindings`; both
  reclaim paths skip them (`ImagePinState::collected`, `IsImageCollected`).
- **uid-paired stored ids**: `Track(slot, uid, id)` / `Lookup(slot, uid)` / `LookupLive(id)` in the
  upscaler, `TryGetImage(id, uid)` everywhere; the member is cleared and the work skipped on a null.
  `Rasterizer::ResetBindings` only clears `binding` for live images; scene-colour detection is
  gated on both images being alive. `TryGetImageTouched(id, uid)` is the public resolve-plus-LRU-touch
  used by the upscaler and camera motion.
- `slot_vector.h` was **not** modified, by instruction: the assertion still stops the GPU, it is only
  preceded by a diagnostic now.

### 4.6 Conservative UWP memory handling ported from GoW3 (same console family)
1. **Marks** (`texture_cache.cpp` ~1183): `trigger = limit − 1280 MiB`, `pressure = limit − 1024 MiB`,
   `critical = limit − 768 MiB` (was −1152 / −768 / −448). On a 5,120 MiB limit that is 3840 / 4096 /
   4352 instead of 3968 / 4352 / 4672 — the pressure line the user watches now shows ≈4096/4352.
2. **`BB_GC_DROP_OLD` defaults off on UWP too** (opt in with `=1`): GoW3 has no tiled-drop path, and a
   dropped target costs a reload that competes for the same limit.
3. **Eviction path** (`clean_up`): a single `keep()` exit for every non-freed candidate — pinned, old
   GPU-written tiled, or below the pressure mark — which touches the LRU entry. The readback resolve
   before the free is `DownloadImageMemory(image_id, /*sync=*/true)`.
4. **`InsertImage` = MakeRoomForImage**: three rounds over unused periods 16 s → 4 s → 2 s
   (64 / 256 / 1024 MiB wants), then `scheduler.Finish()` + `scheduler.PopPendingOperations()` and one
   final round before the `UNREACHABLE_MSG`; the `Finish()` path is last-resort only and logs when it
   fires. `ReclaimMemoryFor` keeps its signature; the age gate moved to private
   `ReclaimMemoryForUnused(addr, size, bytes, unused_seconds)`.
5. **`Rasterizer::OnSubmit()`** calls `buffer_cache.ReleaseRetiredArenas()` under `BB_UWP`, before the
   collector reads app memory. It had never been called — retired dense arenas were never given back.
6. **Stored ids** as in §4.5.

Fence review of (5): `DenseArenaLifetime::CanRelease` requires the 64-tick grace window, the retire
tick to be signalled (`Scheduler::IsFree` on the timeline value the GPU reached) and `Inflight() == 0`;
`OnSubmit` runs after `DrainDrawPipe()` and the guest submission. Added: an arena still referenced by
any `dense_views` entry (a raw `VkBuffer` handed to descriptor sets) is never erased — release is
deferred and logged.

### 4.7 Diagnostics that ship (BB_UWP), cheap enough for release
- `Texture cache: free id=… uid=… by=<eviction|unmap|make-room|other> guest=… size=… bound=… target=…
  proxy=… vo=… tiled=… gpu_modified=… deferred=…` — one line per free, printed from `DeleteImage`,
  the single choke point every free passes through.
- `Texture cache: erase id=… (freed by=…)` — the deferred half, when the slot really dies.
- `Texture cache: DEAD IMAGE id=… indexed by GetImage|GetImageView at file:line (function)` followed by
  `last N frees (oldest first): id(uid,reason) …` — printed when a freed id is indexed, *before* the
  unmodified `slot_vector.h` assertion. `GetImage`/`GetImageView` take a defaulted
  `std::source_location`, so the tag is the real call site with zero changes at the callers.
- `pinned` added to the 5-second pressure line; `GPU: video memory full: three rounds freed … finishing
  the GPU …` when the last-resort path fires.

### 4.8 Test added for the policy
`xbox/tests/texture_cache_policy_test.cpp` — 26 checks, plain C++20, no Vulkan: each pin flag holds an
image, a pinned image costs no deletion, the drawn-but-unbound `collected` case, uid ownership vs
slot reuse vs freed slot. Wired into `xbox.yml`'s test list so it runs on every build.

---

## 5. Build, CI and artifacts

**Toolchain the workflow uses:** windows-2022 runner, MSVC 14.44 for Dozen, clang-cl (LLVM shipped
with VS) for `bloodborne.exe`, DXC 1.9.2609, MSYS2 for third-party deps, `MakeAppx` + `SignTool` for
packaging.

**Windows build chain (run in this order):**

```powershell
powershell -ExecutionPolicy Bypass -File .\xbox\scripts\build-deps.ps1 -Python <python>   # ffmpeg (msvc/clang-cl/lld-link), winpthreads, boost headers
powershell -ExecutionPolicy Bypass -File .\xbox\scripts\build.ps1                          # cmake -S xbox -B xbox\.local\build -G Ninja, clang-cl, RelWithDebInfo -> layout
powershell -ExecutionPolicy Bypass -File .\xbox\scripts\package.ps1                        # BloodborneXbox-<version>.zip
powershell -ExecutionPolicy Bypass -File .\uwp\build.ps1 -Configuration Release -UseClangCL # MSBuild UWP path -> uwp\AppPackages\*.msix
```

**Version string:** `package.ps1` derives `1.0.{YY%100*1000 + dayOfYear}.{HHmm}`, writes it into
`AppxManifest.xml`, the `.appx` name and the zip name. It is **not** printed by `loader.log` (whose
first line is `Bloodborne Xbox Series X|S UWP loader started`); confirm it in the app list /
"Manage app" / the `.appx` filename.

**Workflow triggers:** `push` on `arena/**` and `main`, plus `workflow_dispatch`. A pre-release is
published only when the head commit message contains `[release]` or a dispatch passes `release=true`
(its default). **Re-running a finished run never publishes**: a rerun replays the stored event
payload, so a `push` run without `[release]` in the head commit skips the publish step every time.
Version is generated at package time (`1.0.{YY%100*1000+doy}.{HHmm}`), so each run stamps a new one;
the tag zero-pads the last field (`1.0.26300.211` -> `xbox-alpha-1.0.26300.0211`).

**Last three runs of this work:**

| Run | Result | Note |
|---|---|---|
| [40431164960](https://github.com/youdedend/Test-project/actions/runs/40431164960) | failed | `'TouchImage' is a private member of 'VideoCore::TextureCache'` |
| [40431551643](https://github.com/youdedend/Test-project/actions/runs/40431551643) | failed | `'ReleaseRetiredArenas' is a private member of 'VideoCore::BufferCache'` |
| **[40432005224](https://github.com/youdedend/Test-project/actions/runs/40432005224)** | **green** | build + all unit tests + package; release step skipped |

Artifacts of the green run: `BloodborneXbox-1.0.26300.129` (10,217,374 B), `bloodborne-map-03b74ac…`
(1,436,472 B), `dozen-uwp` (1,691,571 B). Download with `gh run download 40432005224` or from the run
page.

---

## 6. Tests — how to run everything by hand

Portable suites (Linux/macOS/g++ or clang++), each from the repo root:

```bash
g++ -std=c++20 -O1 -Wall -Wextra -Werror -I xbox/platform   -o /tmp/sfo     xbox/tests/sfo_version_test.cpp
g++ -std=c++20 -O1 -Wall -Wextra -Werror -I xbox/platform   -o /tmp/ovl     xbox/tests/overlay_index_test.cpp
g++ -std=c++20 -O1 -Wall -Wextra -Werror -I xbox/platform   -o /tmp/ini     xbox/tests/ini_file_test.cpp
g++ -std=c++20 -O1 -Wall -Wextra -Werror -I xbox/graphics   -o /tmp/arena   xbox/tests/dense_arena_lifetime_test.cpp
g++ -std=c++20 -O1 -Wall -Wextra -Werror -I gpu/shadps4/video_core/texture_cache \
                                                             -o /tmp/policy  xbox/tests/texture_cache_policy_test.cpp
g++ -std=c++20 -O1 -I xbox/platform -o /tmp/dump xbox/tests/dump_layout_test.cpp xbox/platform/prepare_game.cpp
# then run each binary. Current counts: 25, 33, 42, 16, 26, 92 = 234 checks, all pass.
```

`uwp/tests` needs its platform/renderer translation units (the old single-TU command in the appendix
no longer links):

```bash
g++ -std=c++20 -O2 -Wall -Wextra -Werror -I. uwp/tests/test_uwp_port_core.cpp \
    uwp/renderer/d3d12/{D3D12DescriptorManager,D3D12RenderPassCache,SpirvToDxilCompiler}.cpp \
    uwp/BBPort.UWP/Platform/{LifecycleManager,StorageLayer,GamepadInput,XAudio2Sink,UwpFileLoader}.cpp \
    uwp/input/DirectInputShim.cpp -o /tmp/uwp_core && /tmp/uwp_core
# PASS: all BBPort.UWP core platform, input, storage, SFO/ELF, and D3D12 cache tests succeeded.

gcc -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -Isrc tests/test_content.c src/runtime_content.c -o /tmp/tc && /tmp/tc
# PASS: AppContent lifecycle, profile and output boundaries
```

CI runs the six `xbox/tests` suites with clang-cl (`/W3`), then builds and packs.

---

## 7. Compile errors found by CI (and how they were fixed properly)

1. `vk_camera_motion.cpp(347,19)`: `'TouchImage' is a private member of 'VideoCore::TextureCache'`.
   Fixed by adding public `TextureCache::TryGetImageTouched(id, uid)` next to `TryGetImage` — the
   resolve *plus* the LRU touch `GetImage` used to do — and routing the upscaler's `Lookup`/
   `LookupLive` and camera motion's `Overlay` through it.
2. `vk_rasterizer.cpp(1435,18)`: `'ReleaseRetiredArenas' is a private member of
   'VideoCore::BufferCache'`. Fixed by moving that declaration (still `#ifdef BB_UWP`) out of the
   private section, next to `SubmitPendingArenaBinds`.

No assertion was weakened, no diagnostic removed, no feature disabled to get green.

---

## 8. What cannot be done from the agent sandbox (and why)

- **No Windows/UWP build.** Linux only: no `clang-cl`/`cl`/`MSBuild`/`vswhere`, no Windows SDK
  (`windows.h`, `d3d12.h`, `appmodel.h`), no `xbox/.local/deps` (winpthreads, MSVC-target ffmpeg,
  Mesa/Dozen DLLs, DXC), no `xbox/.local/settings.json`, no `makeappx`. CMake 4.4.4 + Ninja 1.13.2
  *can* be installed from PyPI, but the configure stops at
  `CMakeLists.txt:2 (project): The CMAKE_C_COMPILER clang-cl ... was not found in the PATH`.
  `xbox/scripts/build-deps.ps1` builds ffmpeg with `--toolchain=msvc --cc=clang-cl --ld=lld-link`,
  which cannot run on Linux. Every GPU compile therefore happens in CI only.
- **No logs or artifacts can be downloaded here.** GitHub redirects job logs and artifacts to
  `*.blob.core.windows.net`, which is outside the sandbox's allowlist: `gh run view --log`,
  `gh api .../jobs/<id>/logs` and `gh run download` all end in `EOF`. Consequence: **compiler warnings
  in the changed files have never been read.** They are on the run page under "Build Bloodborne Xbox
  UWP" (`warning:`); paste them and they can be fixed in a follow-up.
- **No console run.** Obviously; that is what §9 is for.

---

## 9. Hardware test plan (Series S / X, Developer Mode) — for `1.0.26300.129`

1. Dev Mode → Device Portal → **Add**, pick `BloodborneLauncher_1.0.26300.129_x64.appx` plus the
   `Dependencies/Microsoft.VCLibs.x64.14.00.appx` from the zip. Saves and settings are preserved.
2. Confirm **`1.0.26300.129`** in the app list / "Manage app".
3. Run the same flow that crashed: character creation → tutorial street → Central Yharnam lamp →
   **multiple** area transitions and Hunter's Dream round trips, 10+ minutes. The Dream/area loads are
   where memory peaked before. Note: pressure cannot be forced on console (`BB_GC_BUDGET_MB` is
   `std::getenv`-only and a packaged app cannot set it), so the extra headroom may simply delay the
   collector — a short clean session does not prove anything.

What to look for:

| Log line (grep) | Meaning |
|---|---|
| `Texture cache: memory pressure, … , N pinned` | collector ran; `critical` should now be limit − 768 MiB (≈4352), `pressure` limit − 1024 MiB (≈4096); `pinned` non-zero is expected under load |
| `Texture cache: free … by=eviction\|make-room\|unmap` | who freed an image — the line before a failure names the culprit |
| `Texture cache: erase id=… (freed by=…)` | the slot actually died |
| `Texture cache: DEAD IMAGE id=… indexed by … at file:line` + `last N frees` | the stale id, its call site, and the frees that preceded it |
| `GPU: video memory full: three rounds freed … finishing the GPU` | last-resort path fired (should never appear) |
| `UWP dense GPU arena: … released` / `… still has a DMA view, release deferred` | arena release runs / the new guard is holding one |

`errors.log` must **not** contain `slot_vector.h:225`, `ValidateIndex`, `SlotVector invalid or freed
slot index`, `STOP: GPU library assertion failed`, `ErrorOutOfDeviceMemory`, `Failed allocating image`,
`avplayer_source.h:48`, or any device-lost error.

If it fails, send: full `loader.log` + `errors.log` (plus the previous run's copies), how long after
load, the scene, whether `DEAD IMAGE` appeared and the 20 ids printed after it, the pressure lines
before the stop, and whether it reproduces with the upscaler off.

**The crash is not fixed until this run passes.** A clean run is encouraging, not proof.

---

## 10. Decision and constraint log

- `slot_vector.h`'s assertion is **never** modified; the fix is around it, and diagnostics print before it.
- `third_party/` is not modified.
- Non-UWP behaviour stays as it was unless a change is needed for correctness; UWP-only behaviour is
  under `#ifdef BB_UWP`.
- Public API changes are called out and justified: `TryGetImageTouched`, `HoldCollectedImage` /
  `ClearCollectedImages` / `IsImageCollected`, `TryGetImage`/`IsImageAlive` reuse, `reason` parameters
  on `DeleteImage`/`EraseImage`/`FreeImage`, `ReleaseRetiredArenas` moved to public.
- No push to `main`, no merge to `main`. **Standing instruction (2026-10-27): every change is
  published as a pre-release without asking first.** Either `[release]` in the commit message or a
  `workflow_dispatch` run with `release=true`; the tag is `xbox-alpha-<version>` from the package
  time, target = the branch tip at that moment. This supersedes the earlier "no release unless asked"
  rule.
- The branch name is fixed by the platform (`arena/8ac58bdb-test-project`); it matches the workflow's
  `arena/**` trigger, so pushes build automatically.
- Do not reintroduce anything listed in §11.
- Never claim the crash is fixed without a console run.

---

## 11. Failed attempts — do not reintroduce

Carried forward in full in the appendix ("Failed attempts"): `/STACK:16777216,16777216`;
`DrawPreparation::Build()` returning `{}` without gap-aware lookup; unguarded speculative guest-shader
reads; default-constructed `Shader::ShaderParams`; `scheduler.Finish()` inside `GetDenseArena`;
serializing `getPipelineCacheData()` every 32 PSOs; `.vkcache` sync by file size;
`if_body.pop_front()` in `GotoPass::MoveOutwardIf` / `OpUnreachable` on `BB_UWP`; single-pass
`num_predecessors == 0` erasure; the offline Dozen pipeline pitfalls; relying on VEH for static
`abort()`; swapping non-`WC_GARLIC` memory or running the texture-cache GC inside VMA callbacks. Add
to that list from this round: comparing a `std::list<Buffer>::iterator` with a `const Buffer*`
(`&*it` instead), and calling `TouchImage` / `ReleaseRetiredArenas` from outside their classes before
making them public.

---

## 12. Next steps

1. Console run per §9 with `1.0.26300.129`. If it crashes, the tracing in §4.7 is designed to name
   the exact free that caused it — send the logs.
2. Optionally read the compiler warnings from the green run page and hand them over for a cleanup commit.
3. If the run is clean over long play and multiple area loads, the next milestone is a proper stress
   pass (more distinct areas, Dream round trips, long combat) before calling the crash closed.
4. Releases are no longer a separate step: every push publishes one (§10). A merge to `main`
   (a PR from `arena/8ac58bdb-test-project`) is still not done and still needs a decision.

---

# Appendix — archival record of earlier rounds

Everything below is kept word for word from earlier versions of this handoff. It is the evidence
trail: hardware test numbers, the split-dump/update handling, the character-creation crash
investigation in detail, and the port/review notes from this round. Branch names, PR numbers, commit
hashes and "current state" statements inside it are **historical**; §2 above is authoritative.

## Hardware Test #12

Test #11 (`xbox-alpha-1.0.26299.1047`) crashed on a **01.00** dump at `4135/5120 MiB` (`presents=1376`) with `Fault: read access to 0000020500987170 at bloodborne.exe+0x3003f0` on `shadPS4:GpuCommandProcessor`. Test #12 fixes the eight causes found in that evidence. Nothing from §5 is reopened.

### What changed (one revertable commit per step)

1. **01.09 gate — rolled back.** The prepare-time gate (dialog + exit for `APP_VER != 01.09`, `Prepare: dump_app_ver=...; patches=required`, `Prepared\prepared-format.txt`, dump-version relink and `driver.vkcache` reset, throwing `BuildPatches()`) was reverted on request: `xbox/platform/main.cpp` and `xbox/platform/prepare_game.cpp` are back to the `19d9654` behaviour, so a non-01.09 dump prepares and launches with `Compiled startup patches: game version XX.XX != 01.09; running unpatched 30 FPS` and `Patches: 0 writes`. Steps 2-8 stay in place. Note the consequence: an unpatched guest has no FPS/scene-resolution patch and no red-zone trampolines, which is the configuration Hardware Test #11 ran in.
2. **Symbolication** (`xbox/CMakeLists.txt`, `xbox/scripts/symbolicate.py`, `xbox/scripts/crash-rvas.txt`, `.github/workflows/xbox.yml`): the link now uses `/MAP` (+ explicit `/OPT:ICF`), `symbolicate.py` resolves `bloodborne.exe+0xNNNNNN` RVAs against the map, CI writes the report into the job summary and uploads `bloodborne.map` with every package. **Build 1047 never published a map/PDB**, so its two frames were reconstructed from the fault registers instead: `rcx=0x20500987170`, `rdx=0x204d81fd010`, `rcx-rdx=0x2878A160` is a linear page-table/`cached_pages` index derived from a guest address far outside the 40-bit (1 TiB) space, i.e. **both frames are host `gpu/shadps4` code (page manager / texture page table), not the guest-ELF path** — no "stop and report" condition.
3. **Guest-range guards** (`video_core/page_manager.cpp`, `video_core/multi_level_page_table.h`, `vk_draw_prep.cpp`): `ValidGuestRange()` rejects ranges outside `[0, 1 TiB)` before `cached_pages` is indexed; `MultiLevelPageTable::operator[]` returns a shared sink entry (logging) instead of walking `first_level_map` out of bounds in release builds; the whole per-draw preparation (flat user data, resource sharps, vertex inputs) runs under one `runtime_fault_recover` point and a fault marks the draw `Unavailable`.
4. **Dense-arena refcount** (`xbox/graphics/dense_arena_lifetime.h`, `xbox/graphics/dense_buffers.cpp`, `vk_draw_prep.cpp`, `xbox/tests/dense_arena_lifetime_test.cpp`): a replaced arena is destroyed only when its recorded submissions are signaled **and** no producer holds it; `bb:DrawPrep<N>` retains for the whole prepared submission, `bb:DrawScan` around each scan, and the release happens on the fault path too. Logs `UWP dense arena: retiring deferred (inflight=N, ...)` at most once a second. CI compiles and runs the new unit test.
5. **Swap views** (`xbox/platform/guest_swap.inc`): SRW-locked registry of live `[begin, end)` view intervals; a freshly mapped view must be the requested guest address inside the 40-bit space (debug asserts, release logs and returns `nullptr`); the clock sweep skips pages whose view subpages are no longer all live instead of hashing through a released window; `uwp_swap_protect()` no longer computes a section offset from a released (zero) backing frame.
6. **`driver.vkcache` identity** (`vk_pipeline_cache.cpp`, `cache_storage.{h,cpp}`): logs file and current `vendorID`/`deviceID`/`pipelineCacheUUID` before validating, names the failing check (truncated header / device mismatch / UUID changed), and keeps one generation as `driver.vkcache.rejected`.
7. **Save mounts** (`src/runtime_savedata.c`): mounts are reference counted — a second `sceSaveDataMount` for the same directory returns the same `/savedataN` with status `ALREADY_MOUNTED`, and only the last `sceSaveDataUmount` tears it down (`still in use (refs=N)` otherwise). Fixes the three `SPRJ0005` mounts seen in Test #11.
8. **Copy-layer mismatches** (`vk_runtime.cpp`): on `BB_UWP` a same-type layer mismatch is a soft fault — log the full source/destination `ImageInfo` (format, extent, layers, guest address) once per unique `(format, src_layers, dst_layers)` and skip the copy instead of coercing to `min(layers)`.

### How to run Test #12

1. Install the new `BloodborneXbox-<version>.zip` package (`BloodborneLauncher_<version>_x64.appx` + `Dependencies\Microsoft.VCLibs.x64.14.00.appx`) through Xbox Device Portal, keep `App type = Game`.
2. Point the launcher at any decrypted dump; a **01.09** dump is still the supported/patchable one (`Patches: n writes` with `n > 0`), a different version runs unpatched (30 FPS, `Patches: 0 writes`).
3. Launch, wait for the title screen, then load into Central Yharnam and play past the Cleric Beast area for > 10 minutes (past `presents=10,000`).

### Acceptance for Test #12 (report pass/fail honestly; do not assume)

- `loader.log`: `Prepared ... (app_ver=XX.XX)` and `Compiled startup patches: ...` — with a 01.09 dump that must be `Patches: <n> writes` with `n > 0`; with any other version expect `running unpatched 30 FPS` and `Patches: 0 writes` (step 1 is rolled back by request).
- Startup `AppMemoryUsage` at `presents=0` ≤ `1,100 MiB` (Test #11: `1,001–1,003 MiB`).
- `driver.vkcache` identity line present; the cache is either loaded or discarded **once** with the reason spelled out and `driver.vkcache.rejected` written — a discard on every boot is a failure of step 6 and needs the two UUIDs from the log.
- Yharnam gameplay ≤ `3,500 MiB` (`levels=0..1`), ≥ `25 FPS` after streaming settles.
- `presents > 10,000` with **zero** faults at `+0x3003f0`, `+0x26bbb7`, `+0x27222e`, `+0x272f1c`; `UWP swap: ... corrupt=0` and `window evictions=0`.
- Zero `UWP page table: out-of-range page index` and zero unexplained `refusing view` lines; any `retiring deferred` lines are expected (they mean retirement was safely postponed).
- `GPU: copy layer mismatch: ... skipping the copy` count is reported (each unique triple logs once); it must not correlate with missing visuals.
- `SPRJ0005` mounted once (`Runtime: save data 'SPRJ0005' mounted at /savedata0` followed by `already mounted ... (refs=1)`), never torn down while a session still uses it.
- `check-imports.ps1`, `MakeAppx`, `SignTool`, the dense-arena unit test, and the symbolication report all green in CI.

---

## Update / Patch Handling

The root cause of the `bloodborne.exe+0x3003f0` crash in Hardware Test #11 was not a GPU bug: the
player's USB drive holds the base game and the 01.09 update as **two separate PS4 folders**, the app
only ever read the base folder, so it booted `APP_VER=01.00`, every red-zone patch was skipped and
the guest ran the unpatched 30 FPS build. A dump where the update has been merged into the game
folder (`app_ver=01.09`) was the only configuration that worked. Nothing in §5 is reopened.

This section supersedes the 01.09 gate that was rolled back earlier: there is again a version gate,
but it is derived from what the launcher detects instead of being a hard exit, and it never guesses
a version.

### What the app supports now

| Layout | On disk | Launcher | `Prepare:` line |
| --- | --- | --- | --- |
| merged | one folder, `param.sfo` `APP_VER=01.09` `APP_TYPE=gp` | runs it directly | `effective=01.09; patches=required` |
| split | `Bloodborne\` (01.00) + `Bloodborne-patch\` (01.09, own `eboot.bin`, `sce_module\`, `sce_sys\param.sfo`, `dvdroot_ps4\`) | auto-discovers the sibling, or the `[dump] patch_path` from `bbport.ini`, or the folder the player picks | `base_app_ver=01.00; patch_app_ver=01.09; effective=01.09; patches=required` |
| base only | 01.00 and no update folder anywhere | warning banner; starts only after "Continue anyway (unpatched)" | `effective=01.00; patches=skipped` |

Picking the update folder itself works too: the sibling base folder then supplies the data the
update does not carry.

### Six revertable commits (in order)

1. **`a13b6e0` — param.sfo reader.** `xbox/platform/sfo_version.h` (pure helpers: version ordering,
   `CONTENT_ID …-A0109-V0100 -> 01.09`, `APP_TYPE` classification, the single
   `kPatchTargetVersion`), `SfoInfo`/`ParseParamSfo`/`ReadParamSfo` in `prepare_game.{h,cpp}`.
   Never falls back to a version: an unreadable or `APP_VER`-less file logs the raw header bytes and
   returns `std::nullopt`.
2. **`be9222f` — layout detection.** `DetectDumpLayout(user_picked, preferred_patch)` returns
   `{kind, base_path, patch_path, base, patch, effective_ver}` and logs exactly one line:
   `DumpLayout: kind=split; base_ver=01.00; patch_ver=01.09; effective=01.09; base=…; patch=…`.
   Candidates are rejected (with the reason) on a different `TITLE_ID`, no version, not strictly
   newer than the base, or an incomplete container. `runtime_compat.c` gained
   `bb_uwp_list_dir`/`bb_uwp_is_dir`/`bb_uwp_is_file` so `windows.h` stays out of `prepare_game.cpp`.
3. **`087767f` — launcher panel + persistence.** Base dump / Patch dump / Effective version, the
   "Use patch" toggle in both directions, the base-only banner with "Pick patch folder" and
   "Continue anyway (unpatched)", validation of a manually picked update (readable `param.sfo`, same
   `TITLE_ID`, newer version, `eboot.bin` present) and persistence in **both** places:
   `[dump] patch_path=` / `[dump] use_patch=` in `bbport.ini` and the StorageFolder token `patch` in
   the `FutureAccessList`, redeemed on the next launch. The ini round-trip lives in
   `xbox/platform/ini_file.h` and is CI-tested (comments, order and unrelated settings survive; a
   missing key lands inside the existing section; an unchanged value does not rewrite the file).
4. **`2658229` — the overlay.** `BuildOverlayedImage(base, patch, outDir)` materializes the *boot
   container* into `LocalState\Prepared\overlay` (update first, base game for what the update lacks)
   and writes `Prepared\overlay\files.txt`. `bb_uwp_overlay_configure()` turns that list into a
   sorted index and the platform file layer then serves every `<dump>\<rel>` the update provides
   from `<update>\<rel>` (`bb_uwp_open`/`stat`/`attributes`/`find`, and `opendir`/`readdir` merge the
   update's entries so patch-only files are visible). Game data is never copied — a real dump is tens
   of GB — and neither source folder is ever written. The path rules are pure C in
   `xbox/platform/overlay_index.h` and CI-tested: case- and separator-insensitive prefix match,
   boundary check so a dump named `Bloodborne` cannot swallow a `Bloodborne-patch` sibling, bounded
   sorted lookup.
5. **`a32bc83` — the gate.** `EffectivePatchState()` (required / optional / skipped) from the layout,
   `enable_red_zones` only when not skipped, the single
   `Prepare: base_app_ver=…; patch_app_ver=…; effective=…; patches=required|optional|skipped` line,
   the 01.09 literals in `BuildPatches` replaced by `kPatchTargetVersion`, and
   `xbox/prepared-format.txt` 2 -> 3 so a Prepared folder from the old format regenerates once.
6. **`0be18f1` — overlay.json + cache invalidation.** Boot files are hashed while they are copied;
   `LocalState\Prepared\overlay.json` records the relative paths, the per-file SHA-256 values and a
   `digest` that covers the effective version, the redirect list and the boot bytes but not the
   absolute paths (moving the same dump to another drive is not a change). When the digest differs
   from the previous launch the app logs
   `Prepare: overlay changed (digest … -> …); invalidating Prepared and driver.vkcache` and deletes
   `boot-linked.bin`, `content.bin`, `eboot.elf`, `app_ver.txt`, `format.txt` **and**
   `<user>\cache\<serial>\driver.vkcache` together, because they describe the same guest build.

### Expected lines in `loader.log.txt` for each layout

```
# merged 01.09
DumpLayout: kind=merged; base_ver=01.09; patch_ver=-; effective=01.09; base=E:\dumps\Bloodborne; patch=-
Prepare: base_app_ver=01.09; patch_app_ver=-; effective=01.09; patches=required

# split (base 01.00 + Bloodborne-patch 01.09)
DumpLayout: kind=split; base_ver=01.00; patch_ver=01.09; effective=01.09; base=E:\dumps\Bloodborne; patch=E:\dumps\Bloodborne-patch
Overlay: E:\dumps\Bloodborne + E:\dumps\Bloodborne-patch -> D:\…\LocalState\Prepared\overlay (N files from the update, M copied, N redirected, digest …)
Update overlay: N paths from E:\dumps\Bloodborne-patch over E:\dumps\Bloodborne
Prepare: base_app_ver=01.00; patch_app_ver=01.09; effective=01.09; patches=required

# base only
DumpLayout: kind=base-only; base_ver=01.00; patch_ver=-; effective=01.00; base=E:\dumps\Bloodborne; patch=-
Prepare: base_app_ver=01.00; patch_app_ver=-; effective=01.00; patches=skipped
Prepare: red-zone patches are skipped, the game runs the unpatched build (as confirmed by the player)
```

### How to verify

1. Install the package, launch once with the **merged** 01.09 dump: the panel says
   `Effective version: 01.09`, `loader.log` has `patches=required` and
   `Compiled startup patches` with `n > 0`.
2. Copy the **split** pair to USB (`E:\dumps\Bloodborne` + `E:\dumps\Bloodborne-patch`), point the
   launcher at the base folder, accept the detected update: the panel must show both paths and
   `Effective version: 01.09`; afterwards `SHA-256` of both USB folders must be unchanged.
3. Restart: the choice must survive (`[dump] patch_path=` in `LocalState\bbport.ini`), the panel must
   come up already patched, and `Prepare:` must still say `patches=required`.
4. Pull the update folder away and restart: the panel must show the base-only banner; the game may
   only start after "Continue anyway (unpatched)" and must then log `patches=skipped`.

### Still open

- **Hardware run pending.** Nothing here has been exercised on a Series S yet. The acceptance for
  this work is the split-dump run: `presents > 10,000` in Yharnam with zero faults at `+0x3003f0`
  and `effective=01.09; patches=required` in the log, plus the three launcher-panel states and the
  before/after `loader.log` excerpts for the PR description.
- **Do not merge PR #10 before that run passes.**
- `driver.vkcache` is dropped whenever the overlay digest changes; if a future update is expected to
  keep the cache, the digest would have to be split into "guest build" and "data set" parts.

### CI history for this work

| Run | Head | Result |
|---|---|---|
| `40389855254` | `0be18f1` | `Dozen for UWP` ok, `Xbox UWP package` **failed** in "Build and run UWP platform unit tests" |
| `40391383955` | `a7cb3de` | same step failed; the annotation (first 4096 chars of the log) hid the failing check |
| `40395148890` | `dc2bd97` | **green**, both jobs |

Two real defects came out of that loop, and one workflow improvement:

1. **`a7cb3de`** — the test step published the raw tail of the compiler/test output as a check-run
   annotation. GitHub keeps only the first 4096 characters, so the `FAIL:` line (which comes first)
   was cut off. `Publish()` now keeps only lines matching
   `FAIL|FATAL|error:|warning:|check(s) failed|did not compile` (falling back to the last 40 lines)
   and takes the first 3800 characters, so the failing check is what survives.
2. **`c344a14`** — `SfoInfo::IsPatch()` treated the `CONTENT_ID`'s update group as decisive, so a
   **merged** 01.09 dump (`app_type=gp`, `content_id=…-A0109-V0100`) reported `kind=patch` and the
   layout decisions mis-classified a base game. It is now
   `IsPatchAppType(app_type) || (app_type.empty() && !update_ver.empty())` — the `CONTENT_ID`
   fallback only applies when there is no `APP_TYPE` at all — and both `DetectDumpLayout`
   decisions call `IsPatchAppType(info->app_type)` directly. The diagnostic line became
   `Prepare: param.sfo kind=%s (app_type=%s)`.
3. **`dc2bd97`** — three `dump_layout` checks compared a normalized path with a raw
   `std::filesystem` path. That is a no-op on Linux (`/` on both sides) but fails on Windows, where
   `std::filesystem` hands back `\`. Both sides now go through the same normalizer.

### The panel text is testable too

`main.cpp` owns the WinRT dialogs and cannot be built on the CI machine, so every sentence the
player sees and every summary line in `loader.log.txt` now comes from `xbox/platform/dump_panel.h`
(`BbPanel::DescribeDump`, `PanelText`, `BaseOnlyBanner`, `LaunchTargetLine`, `PrepareLine`,
`SkippedPatchesLine`, `OverlayChangedLine`). `main.cpp` keeps only the dialog plumbing, and
`dump_layout_test.cpp` pins the three launcher states verbatim and prints them, so the states and the
`Prepare:`/`Launch target:` lines appear in the CI log instead of only on a console.

### Status after the CI loop (branch `arena/c39edde2-test-project`)

Twelve commits on top of the pre-mission branch head, all pushed, all compiled and tested by CI
(followed by the documentation commits that carry this section):

```
7a6ef60  Pin the launcher panel text and the Prepare lines in CI
21b0d3a  Record the CI loop that found the APP_TYPE and path-comparison defects
dc2bd97  Compare dump paths after normalizing both sides in the CI test
c344a14  Classify a container by APP_TYPE, not by its CONTENT_ID update group
a7cb3de  Report UWP platform unit test failures as CI annotations
d94cfdc  Document update/patch handling in handoff.md
0be18f1  Record the overlay in overlay.json and invalidate both caches when it changes
a32bc83  Gate the red-zone patches on the version that will actually run
2658229  Lay a separately dumped update over the game dump without copying it
087767f  Show the dump layout in the launcher and remember the patch folder
be9222f  Detect merged, split and base-only dumps instead of guessing
a13b6e0  Read APP_VER/APP_TYPE/TITLE_ID/CONTENT_ID from param.sfo, never guess a version
```

(`a13b6e0` … `0be18f1` are the six steps of this task, `d94cfdc` is the first version of this
section, `a7cb3de`/`c344a14`/`dc2bd97` are the CI annotation fix and the two real defects the loop
found, `21b0d3a` and the documentation commits after `7a6ef60` are this section, and `7a6ef60`
is the panel-text test.)

| Run | Head | Result |
|---|---|---|
| `40389855254` | `0be18f1` | `Dozen for UWP` ok, `Xbox UWP package` failed in "Build and run UWP platform unit tests" |
| `40391383955` | `a7cb3de` | same step, annotation still unreadable (see below) |
| `40395148890` | `dc2bd97` | **green**, both jobs |
| `40396701707` | `21b0d3a` | **green**, both jobs (documentation only) |
| `40399498409` | `7a6ef60` | **green**, both jobs |

What the failing annotation actually said, once `Publish()` stopped publishing the log tail:

```
FAIL: the preferred patch path is kept (got 'C:/Users/RUNNER~1/AppData/Local/Temp/bb_dump_layout_test/elsewhere/MyPatch', expected 'C:\Users\RUNNER~1\AppData\Local\Temp\bb_dump_layout_test\elsewhere\MyPatch')
FAIL: the sibling base folder supplies the data (got 'C:/Users/RUNNER~1/AppData/Local/Temp/bb_dump_layout_test/rooted/Bloodborne', expected 'C:\Users\RUNNER~1\AppData\Local\Temp\bb_dump_layout_test\rooted\Bloodborne')
FAIL: the picked patch folder is the executable source (got 'C:/Users/RUNNER~1/AppData/Local/Temp/bb_dump_layout_test/rooted/Bloodborne-patch', expected 'C:\Users\RUNNER~1\AppData\Local\Temp\bb_dump_layout_test\rooted\Bloodborne-patch')
3 check(s) failed
```

i.e. the *test* was wrong on Windows, not the launcher — which is why the fix (`dc2bd97`) normalizes
both sides instead of changing `DetectDumpLayout`. The lesson worth keeping: a check-run annotation
is truncated to the first 4096 characters, so publish the filtered failure lines, not the tail.

Local test results after `7a6ef60` (the same binaries CI builds):

| Binary | Checks |
|---|---|
| `dump_layout_test` (links the real `prepare_game.cpp`) | **92** — 75 layout/overlay/manifest + 17 panel/`Prepare:`-line |
| `ini_file_test` | 42 |
| `sfo_version_test` | 25 |
| `overlay_index_test` | 33 |
| `dense_arena_lifetime_test` | pass (unchanged) |

```
g++ -std=c++20 -I xbox/platform xbox/tests/dump_layout_test.cpp xbox/platform/prepare_game.cpp \
    -o /tmp/dlt && /tmp/dlt
g++ -std=c++20 -I xbox/platform xbox/tests/ini_file_test.cpp -o /tmp/ini && /tmp/ini
g++ -std=c++20 -I xbox/platform xbox/tests/sfo_version_test.cpp -o /tmp/sfo && /tmp/sfo
g++ -std=c++20 -I xbox/platform xbox/tests/overlay_index_test.cpp -o /tmp/ovl && /tmp/ovl
```

The launcher states are now part of the test output, which is what the CI log carries:

```
--- launcher panel, merged dump ---
Base dump:  E:\dumps\Bloodborne - app_ver=01.09 (gp) CUSA03173
Patch dump: none found
Effective version: 01.09

--- launcher panel, split dump ---
Base dump:  E:\dumps\Bloodborne - app_ver=01.00 (gp) CUSA03173
Patch dump: E:\dumps\Bloodborne-patch - app_ver=01.09 (patch) CUSA03173 (in use)
Effective version: 01.09

--- launcher panel, base only ---
Base dump:  E:\dumps\Bloodborne - app_ver=01.00 (gp) CUSA03173
Patch dump: none found
Effective version: 01.00 - patches will be SKIPPED

! No 01.09 update folder was found.
Without it the red-zone patches are skipped: the game runs unpatched at 30 FPS and can crash when
gameplay starts.

Choose the update folder - the one holding eboot.bin and sce_sys\param.sfo with APP_TYPE=patch
(usually next to the game folder: E:\dumps\Bloodborne-patch).

The game folder itself is never modified.
```

These are produced by the shipped string builders (`xbox/platform/dump_panel.h`) on the CI machine,
**not** on a console: the dialogs themselves are WinRT and only exist in the app.

### What is still missing before this can merge

1. **The Series S run with a split 01.09 dump** — `presents > 10,000` in Yharnam, zero faults at
   `+0x3003f0`, and `effective=01.09; patches=required` in `loader.log.txt`. Nothing in this section
   has been exercised on hardware yet; the CI machine cannot boot the game.
2. **The three launcher screenshots and the before/after first 15 lines of `loader.log.txt`** for the
   split case — they need that run. Until then the excerpts above are the honest substitute: the
   exact text, from the exact code, without a console behind it.
3. `PR #10` must not be merged before (1) passes. The PR description now carries the layout table,
   the three panel states, the overlay design, the verification table and the CI-loop table.

### Final state of this branch

`HEAD` is documentation only from here on; every code commit below has a green CI run.

| Run | Head | Both jobs |
|---|---|---|
| `40389855254` | `0be18f1` | failed in "Build and run UWP platform unit tests" |
| `40391383955` | `a7cb3de` | same step |
| `40395148890` | `dc2bd97` | **green** |
| `40396701707` | `21b0d3a` | **green** |
| `40399498409` | `7a6ef60` | **green** |
| `40402529867` | `6019252` | **green** (handoff.md only) |
| `40402540016` | `7d17bf0` | **green** (handoff.md only) |
| `40402572925` | `30ca026` | **green** (handoff.md only) |
| `40402609734` | `2f6c410` | **green** (handoff.md only) |

#### Findings verified in the code, not assumed

- **The dump is never written.** Every write the launcher performs goes to `LocalState`: `bbport.ini`,
  `Prepared\boot-linked.bin` / `content.bin` / `eboot.elf` / `app_ver.txt` / `format.txt` /
  `patches.bin` / `overlay\*` / `overlay.json`, the crash quarantine file and `dozen.log`. There is no
  `bb_uwp_mkdir`/`CreateFile2FromAppW` call against the game or update folder anywhere in
  `xbox/platform/main.cpp`, which is what makes the "USB dump byte-identical" acceptance testable
  with a SHA-256 of both folders before and after.
- **The update is not copied.** `BuildOverlayedImage` copies only the boot container (`eboot.bin`,
  `sce_sys\param.sfo`, `sce_module\*.prx` and other root/`sce_*` files) — a handful of megabytes — and
  writes every other path the update provides into `Prepared\overlay\files.txt`, which
  `bb_uwp_overlay_configure()` turns into a redirect in the platform file layer. A 10 GB
  `dvdroot_ps4` never lands in `LocalState`.
- **01.09 is written down once.** `kPatchTargetVersion` in `xbox/platform/sfo_version.h` is the only
  definition (8 uses in non-test code); the string `"01.09"` appears elsewhere only in two comments in
  `prepare_game.h`. A future 01.10 update changes one line.
- **The launcher text has one source.** Every sentence in a dialog and every summary line in
  `loader.log.txt` comes from `xbox/platform/dump_panel.h`; `main.cpp` only wraps it in
  `MessageDialog`/`printf`. That is why the three states can be pinned in CI at all.
- **No new capabilities.** The patch folder is remembered through the same
  `StorageApplicationPermissions::FutureAccessList` token mechanism as the game folder (token `patch`,
  redeemed into `g_patch_folder_lease`), so `runFullTrust`/`broadFileSystemAccess` stay absent and
  `check-imports.ps1` still passes.

#### Still blocking the merge

1. Series S run with a split 01.09 dump: `presents > 10,000` in Yharnam, zero faults at
   `+0x3003f0`, `effective=01.09; patches=required`.
2. The three launcher screenshots and the before/after first 15 lines of `loader.log.txt` from that
   run (the excerpts above are the same strings from the same code, produced on the CI machine).
3. `PR #10` description carries the layout table, the three panel states, the overlay design, the
   verification table and the CI-loop table; it must not be merged before (1).

---

## Crash after character creation — what the PC-port reports translate to

The same crash is a well-documented PC problem. Screened against this codebase, one of the community's
reported causes is a **verbatim match**, one is **already implemented here**, and the rest are N/A.

| PC cause / community fix | Status in this port |
| --- | --- |
| `avplayer_source.h:48 Assertion Failed! Could not allocate frame buffer` — crash on the first movie after creation; people work around it with a post-creation save or by lowering effects | **Was a real defect here.** `GuestBuffer` called `ASSERT_MSG(m_data, ...)`, so a failed guest allocation aborted the process (dashboard crash) instead of failing the stream. **Fixed.** |
| Enable GPU readbacks, or disable FaceGen (vertex explosions / stale vertices) | **Already implemented**: `EmulatorSettings.GetReadbacksMode()` defaults to `Relaxed` (`gpu/shim/core/emulator_settings.h`), honoured by `RegionManager::InvalidateRegion` / `ForEachModifiedRange`. No FaceGen patch needed. |
| Out of device memory (`ErrorOutOfDeviceMemory`), 4 GB VRAM, RAM spikes | Still the dominant risk for us: Test #11 died at `4135/5120 MiB`. The `AppMemoryUsage` work (GC at frame boundaries, dense-arena retirement, `/STACK:8388608,65536`) is the equivalent of their "RAM spike workaround" mods. |
| `SDL_audiocvt.c ConvertAudio` buffer overrun / LibAtrac9 overflow on the cutscene audio | **N/A**: audio is FFmpeg + `XAudio2`; there is no SDL and no `LibAtrac9` call site in `gpu/` or `src/`. |
| Toggling patches (`Skip Intro`, 60 FPS, resolution) changes whether it crashes | Maps to our `patches/redzone.json` / `Bloodborne.xml` state: a `01.00` dump runs with `patches=skipped`, which is the configuration Test #11 crashed in. Split-dump support addresses this. |
| VBlank divider / frame-rate cap causing the load to hang | Exposed as `BB_VBLANK_HZ` / `BB_FPS_LIMIT`, not a defect. |

### The fix (`gpu/shadps4/core/libraries/avplayer/`)

- `GuestBuffer::GuestBuffer` no longer aborts. It logs the size/align and exposes `IsValid()`.
- `AvPlayerSource::Init` allocates video and audio frame buffers one at a time, stops on the first
  refusal, and **fails the stream** (`return false`) when fewer than two are available — the count
  below which the decoder and presentation callback cannot run. The guest receives an AvPlayer error
  and keeps running, instead of the app dying on the cutscene after character creation.
- Partial allocation still plays: `N of M frame buffers allocated, playing with N`.

What to look for in `dozen.log` / `errors.log` on the next hardware run:
`Could not allocate frame buffer (… bytes, align …, texture=…)` followed by
`Video '<path>': only N of M frame buffers … could be allocated` — that is the memory-exhaustion
pressure the PC reports describe, now visible instead of fatal.

---

## The crash after character creation, second round (`1.0.26299.2359` logs)

Test #13 ran the split dump correctly — `DumpLayout: kind=split; base_ver=01.00; patch_ver=01.09;
effective=01.09; patches=required`, `Compiled startup patches: FPS=60, ... writes=196`, then died.
The reported symptom ("exact same crash") is correct, but it is **not** the AvPlayer path fixed in
`a416e62`:

```
Texture cache: memory pressure, 4493 of 4352 MiB (critical 4672): 1 images evicted ...
GPU [Debug] <Critical> slot_vector.h:225 ValidateIndex: Assertion Failed!
SlotVector invalid or freed slot index=132 capacity=2978      (previous run: 257)
STOP: GPU library assertion failed
```

### Cause

`Common::SlotId` is a bare `u32` index with **no generation counter**. The rasterizer resolves every
texture binding in two passes: pass 1 collects `(image_id, desc)`, pass 2 calls `GetImage(image_id)`
and `FindTexture(image_id, ...)`. Under memory pressure `InsertImage` -> `ReclaimMemoryFor` ->
`DeleteImage` frees images *between* those passes (the log line above is that eviction), so a
collected `image_id` can already be dead in pass 2. Indexing with it either trips `ValidateIndex`
(the crash seen) or, if the slot is reused first, silently binds the **wrong image**.

Two more places hold ids across an erase: `TextureCache::FindTexture`'s `ViewMemo` (kept in the
rasterizer's binding entry, which outlives the image) and `Image::last_view` / `image_view_infos`,
which keep indexes into `slot_image_views` after `DeleteImage` reset and erased those views.

### Fix

- `TextureCache::IsImageAlive(ImageId)` — `slot_images.is_allocated`, next to the existing `TryGetImage`.
- `vk_rasterizer.cpp`, binding pass 2: re-resolve a dead id with `FindImage(desc)` (same guest
  address, so the draw stays correct and the texture is simply uploaded again) and fall back to
  `NullView()` if even that fails; the `needs_rebind` path is re-checked after it re-resolves, since
  `FindImage` can evict while resolving.
- `FindTexture`: the memoised view is only used while `slot_image_views.is_allocated(memo->view_id)`.
- `Image::FindView`: `last_view` and the `image_view_infos` search only return a view whose slot is
  still allocated; a stale list entry is overwritten with the freshly created view so the list does
  not grow without bound.

Logs to look for next run: `GPU: image binding N was evicted before it was bound; re-resolving
addr=... size=...` (at most 8 times). Those lines replacing the assertion mean the eviction race was
the cause; if `slot_vector.h:225` still fires, the id in the message tells us which container and
path, since neither `FindTexture` nor `FindView` can reach it any more.

### Second cause: the collector freed images that were still held (this round)

The `1.0.26300.x` run stops in the same assertion, but the free that caused it is a *different* path
than the two-pass binding race above:

```
Texture cache: memory pressure, 4493 of 4352 MiB (critical 4672): 1 images evicted
slot_vector.h:225 ValidateIndex: Assertion Failed!
SlotVector invalid or freed slot index=132 capacity=2978
STOP: GPU library assertion failed
```

`GarbageCollectImages`' `clean_up` lambda decided purely on `SafeToDownload()`, `IsTiled()` and the
LRU age. It never asked whether the image was still in use, while `ReclaimMemoryFor` has always
skipped `info.scene_proxy`, `usage.vo_surface`, `binding.is_bound` and `binding.is_target`. Under
pressure (4 GB console memory) a bound render target or the video-out surface the presenter reads
could therefore be evicted, and the rasterizer / presenter / upscaler then indexed the freed slot.

The second half is that `Common::SlotId` has no generation, so even a *correctly* pinned image can be
freed later (UnmapMemory) behind a stored id: `GetImage` on it either asserts or silently returns
whichever image reused the slot.

Fix:

- `video_core/texture_cache/texture_cache_policy.h` (new, no Vulkan): `BbGpu::ImagePinState`,
  `ImagePinned`, `GcMayFreeImage`. `clean_up` now returns `false` for a pinned candidate without
  spending a deletion, and touches its LRU entry so the next pass is not stuck on the same entry.
  A `pinned` count is added to the 5-second pressure line, `gc_pinned`.
- Stale ids now carry their `image_uid`: the upscaler's `scene_color` / `ldr_target` / `ui_color` /
  `ui_depth` are stored through `Track(slot, uid, id)` and read through `Lookup(slot, uid)`
  (`TryGetImage(id, uid)`) or `LookupLive(id)` for ids owned by another component; every read site
  bails out instead of indexing. `OnDraw` also drops `color`/`depth` parameters that are already
  dead. `CameraMotion::Overlay` and `Rasterizer::ResetBindings` do the same with
  `TryGetImage(id, 0)` / `IsImageAlive`.
- `BB_GC_LOG_FREES=1` (UWP builds) traces every free with `id uid guest size bound target proxy vo
  tiled gpu_modified now`, to match a next-run assertion against the image that was freed.

Not changed: `TextureCache::UnmapMemory` still frees every image in the unmapped guest range, since
the guest really did unmap them; the holders now tolerate that instead.

How to verify: `xbox/tests/texture_cache_policy_test.cpp` (CI test, plain C++20, compiles with
`g++ -std=c++20 -I gpu/shadps4/video_core/texture_cache`) pins the policy and the uid rule. On the
console, a next log should show `pinned` in the pressure line and, with `BB_GC_LOG_FREES=1`, which
image was freed right before any remaining assertion. **This is not confirmed until it runs on a
Series S / X through character creation and into Central Yharnam.**


---

## Failed attempts — original list, verbatim

This is §5 of the old handoff, kept so nothing is lost. §11 above summarises it and adds
two entries from the latest round.

Do **not** reintroduce any of the following approaches:

1. **Linking with `/STACK:16777216,16777216` (`xbox/CMakeLists.txt`)**:
   - Setting `SizeOfStackCommit = 16 MiB` in the PE header forces Windows `CreateThread` / `_beginthreadex` to commit `16 MiB` of physical RAM per OS thread (`82 × 16 MiB ≈ 1,312 MiB`), overriding `pthread_attr_setstacksize` and starving the `5,120 MiB` Series S budget. Keep `/STACK:8388608,65536`.
2. **Returning `{}` from `DrawPreparation::Build()` without gap-aware lookup/rebase (`vk_draw_prep.cpp`)**:
   - `Liverpool::SubmitGfx()` unconditionally increments `gfx_submit_seq++`. Any skipped `Build()` creates non-contiguous `seq` numbers in `submissions`, causing `submissions[next_seq - submissions.front()->seq]` in `ScannerLoop`/`WorkerLoop`/`BeginSubmission` to index out of bounds (`bloodborne.exe+0x270628`) and desynchronizing `tail_regs` deltas.
3. **Unguarded speculative guest-shader reads on `bb:DrawPrep0` (`vk_pipeline_cache.cpp` / `regs_shader.h`)**:
   - Calling `AmdGpu::GetParams(*pgm)` when `sel.worker != nullptr` dereferences guest shader memory without `runtime_fault_recover` and aborts via `UNREACHABLE_MSG` if the guest reused that page ahead of the GPU thread. Always use `runtime_fault_recover` + `AmdGpu::TryGetParams(*pgm)`.
4. **Default-constructing `Shader::ShaderParams params{}` or block-scope `extern "C"`**:
   - `Shader::ShaderParams::user_data` is `std::span<const u32, NumShaderUserData>` (fixed extent != `std::dynamic_extent`), which has no default constructor in C++20/23 (`clang-cl` error); always wrap deferred initialization in `std::optional<Shader::ShaderParams>`. Likewise, `clang-cl` rejects `extern "C"` declarations inside function bodies (`[dcl.link]/4`); keep them at namespace scope.
5. **Calling `scheduler.Finish()` inside `BufferCache::GetDenseArena()` (`xbox/graphics/dense_buffers.cpp`)**:
   - `GetDenseArena` runs inside `ObtainBuffer` -> `ResolveVertexBuffers` *after* `BeginRendering`. Calling `scheduler.Finish()` there splits and submits an active render pass mid-draw. Only call `scheduler.PopPendingOperations()` inside `GetDenseArena`, and run `scheduler.Finish(); scheduler.PopPendingOperations();` at frame boundaries inside `TextureCache::RunGarbageCollector()`.
6. **Serializing `getPipelineCacheData(*pipeline_cache)` every 32 runtime PSOs or when `num_flushed_pipelines == 0`**:
   - Allocates a temporary `~174 MiB` `std::vector<u8>` and queues a `174 MiB` SSD write mid-gameplay, pushing `AppMemoryUsage` across thresholds.
7. **Syncing `.vkcache` solely by file size (`src_size > dst_size`)**:
   - Overwrites valid local caches across Mesa Dozen rebuilds or skips valid exports when `pipelineCacheUUID` changes; always validate the 32-byte `VkPipelineCacheHeaderVersionOne` + `pipelineCacheUUID`.
8. **Keeping `if_body.pop_front()` in `GotoPass::MoveOutwardIf` or emitting `OpUnreachable` on `BB_UWP`**:
   - `if_body.splice(if_body.begin(), body, std::next(goto_stmt), body.end())` already excludes `goto_stmt`; calling `if_body.pop_front()` unlinks the empty `boost::intrusive::list` sentinel when `std::next(goto_stmt) == body.end()` (crashing shader `i = 1024`) and drops statements otherwise. Also emit `OpReturn` instead of `OpUnreachable` on `BB_UWP` because Mesa Dozen's `nir_to_dxil.c` (`emit_jump`) only handles `break`/`continue`.
9. **Single-pass `num_predecessors == 0` erasure in `CFG::RemoveUnreachableBlocks()`**:
   - Leaves unreachable loops/diamonds intact (`num_predecessors >= 1`), crashing Mesa's `vtn_cfg.c`. Always compute DFS reachability from the entry block and recompute `num_predecessors` over reachable edges only.
10. **Offline Dozen `GraphicsPipeline` pitfalls (`bb_precompile.cpp`)**:
    - Never compile color-pass `VS` standalone at `binding = 0` (`dzn_pipeline.c` `adjust_var_bindings` asserts `b < binding_count`); never leave depth-only `z_format` as `0` (`ZFormat::Invalid` skips shader compilation—use `ZFormat::Z32Float`); force `rasterizationSamples = e1` when `preloading == true`; cap VS attributes to `<= 28` (`semantic < 32`); ensure passthrough VS writes `gl_Position`; and cap `rt.hw.vs.num_outputs` to `0..3`.
11. **Relying on VEH / `signal(SIGABRT)` for static UCRT `abort()` or using `bb_longjmp` across C++ frames**:
    - Static `libucrt.lib` `abort()` executes `__fastfail` (`int 0x29`), bypassing Win32 VEH handlers. Crash-resilient precompilation must flush `LocalState\precompile_inflight.txt` before compiling and consume it on startup.
12. **Swapping `ONION`/`guest_code` memory, calling `TextureCache` GC inside VMA `createImage`, or committing after container refresh without checking `HEAD`**:
    - Restrict swap strictly to `WC_GARLIC` with fault-time guest pinning; run `TextureCache` GC only at frame boundaries outside `TextureCache::mutex`/VMA callbacks; build Mesa Dozen with MSVC `14.44` (never `19.51`, which ICEs on Mesa); never use `gh release download` in sandbox (`release-assets.githubusercontent.com` is outside the proxy allowlist); and after any container refresh, verify `git log -n 2` against `origin/arena/c39edde2-test-project` (`git fetch` + `git reset --soft origin/arena/c39edde2-test-project && git reset`) before committing.

---

