# Bloodborne Xbox Series X|S UWP Port — Engineering Handoff

## 1. Goals

Build and ship a complete, fully playable Xbox Series X|S Developer Mode UWP port of `bloodborne_pc` (`Supermedo/bloodborne_pc`, `windows` baseline `5adea35dbd38`) in `youdedend/Test-project`, compiled and signed end-to-end inside GitHub Actions so a user can install the `.appx` on an Xbox console, supply their own decrypted PS4 Bloodborne `v01.09` dump (`eboot.bin`, `sce_module`, `sce_sys`, `dvdroot_ps4`), and play the game natively on hardware.

Core acceptance criteria and standing constraints:

- **Native CPU + HLE + GPU Execution**:
  - Compile the PS4 x86-64 ELF loader (`src/probe.c`) and HLE kernel/libc/filesystem/thread/sync/pad/audio runtime (`src/runtime*.c`) with `clang-cl` (`sysv_abi` bridge) alongside the `shadPS4` Liverpool GPU emulator (`gpu/shadps4/**`).
  - Run Vulkan rendering on Xbox Series X|S Direct3D 12 via packaged Mesa Dozen (`vulkan_dzn.dll` built from `Cosmo/mesa_ai_slop_version` @ `a76cd3caa23f14839f15259cd3abbfa400b1b217` with MSVC `14.44`), DirectX Shader Compiler `v1.9.2609` (`dxil.dll`), and a native D3D12 `CoreWindow` flip-model swapchain presentation bridge (`xbox/graphics/present_bridge.cpp`).
- **In-App Native C++ Dump Preparation (Zero Python Dependency on Console)**:
  - Replicate the desktop Python preparation pipeline (`prepare.py`, `link_modules.py`, `link_libc.py`, `cpu_compat.py`, `patches.py`, `content_profile.py`, `patches/redzone.json`, `patches/Bloodborne.xml`) natively in `xbox/platform/prepare_game.cpp` (`prepared-format.txt` = `2`), enforcing `APP_VER = 01.09` whenever `redzone.json` targets `01.09` offsets and emitting `LocalState\prepared\boot-linked.bin`.
- **Unified `5,120 MiB` Xbox Game Mode Memory Architecture**:
  - Fit PS4 guest memory, `boot-linked.bin`, `shadPS4` buffer/texture caches, Mesa Dozen/D3D12 heaps, and `VkPipelineCache` comfortably inside the `5,120 MiB` Xbox Series S|X Game Mode AppContainer cap (`AppMemoryUsageLimit`).
  - Guarantee `< 1 TiB` (`< 0x10000000000`) virtual addresses for all guest-visible allocations, stacks, TLS blocks, GOT tables, and media buffers; back GPU page-tracking via `VirtualAlloc2FromApp` / `MapViewOfFile3FromApp`; and swap cold `WC_GARLIC` pages cleanly to a sparse file in `LocalState` without evicting CPU-touched pages.
- **Crash-Free Ahead-of-Gameplay Shader & Pipeline Precompilation**:
  - Parse Bloodborne `.shaderbnd.dcx` (`BND4` + `DCX` deflate) archives plus `boot-linked.bin` / DLC / mod directories ahead of gameplay (`gpu/shim/bb_precompile.cpp`), pre-build linked `(VS, PS)`, depth-only `VS`, and `RectList`/`QuadList` geometry pipelines into Mesa Dozen's `driver.vkcache`, recover automatically from crashes via disk breadcrumbs, and synchronize caches across `LocalState` and USB dumps.
- **Complete Console UX & I/O**:
  - Support `Windows.Gaming.Input.Gamepad` (buttons, analog sticks, triggers, rumble, `L3 + R3` ImGui settings overlay, touchpad gestures, and controller virtual keyboard for Hunter Name entry), `XAudio2` 7.1/stereo output, `LibAtrac9`/FFmpeg `AvPlayer` decoding, and dump discovery from `D:\DevelopmentFiles\Bloodborne`, `LocalState\game`, or USB drives (`removableStorage` + `FutureAccessList` + `fileTypeAssociation`).
- **Strict UWP & Legal Compliance**:
  - Zero `runFullTrust` or `broadFileSystemAccess` declarations in `xbox/AppxManifest.xml`.
  - Every PE binary verified by `xbox/scripts/check-imports.ps1` against `WindowsApp.lib`, the Store CRT, or packaged DLLs.
  - Zero copyrighted Sony binaries, `eboot.bin`, keys, ISOs, or private credentials committed to Git (test signing uses the purposefully public self-signed `xbox/signing/bloodborne_xbox-signing.pfx` for Xbox Developer Mode sideloading).

---

## 2. Current state

- **Repository & Branch**:
  - Repo: `https://github.com/youdedend/Test-project` (`/home/user/Test-project`)
  - Active session branch: `arena/c39edde2-test-project` (branched from `main` @ `cd63381710b002818a91aa4618eeb1b1ca68b410`)
  - Open Pull Request: **PR #10** (`arena/c39edde2-test-project` -> `main`) — `https://github.com/youdedend/Test-project/pull/10` (`status: OPEN`)
- **Latest Verified CI Build & Signed Pre-Release**:
  - Code commit: `a3d39fd` (`fix(xbox): use std::optional<ShaderParams> for fixed-extent user_data span [release]`, immediately following `9b74bb3`)
  - GitHub Actions run: **`40309332456`** — both `Dozen for UWP` and `Xbox UWP package` completed **green** (including `clang-cl` build, `check-imports.ps1` Store/AppContainer import audit, `MakeAppx`, `SignTool`, and release upload).
  - Pre-release tag: **`xbox-alpha-1.0.26299.1047`** (`https://github.com/youdedend/Test-project/releases/tag/xbox-alpha-1.0.26299.1047`)
  - Downloadable bundle: `https://github.com/youdedend/Test-project/releases/download/xbox-alpha-1.0.26299.1047/BloodborneXbox-1.0.26299.1047.zip` (`10,101,375` bytes; contains `BloodborneLauncher_1.0.26299.1047_x64.appx` + `Dependencies/Microsoft.VCLibs.x64.14.00.appx`).
- **Console Hardware Validation State**:
  - Validated on real Xbox Series S hardware (`5,120 MiB` Game Mode) across 10 hardware test iterations through `xbox-alpha-1.0.26299.0959` (`9d261ad`):
    - In-app C++ preparation (`boot-linked.bin`), offline `.shaderbnd.dcx` precompile (**`driver.vkcache` = `173,715 KiB` / `169.6 MiB`**), startup `WarmUp` (**`121/121` pipelines**), `XAudio2` sound, controller input, Hunter Name creation, and save-data mount (`SPRJ0005`) all succeeded on hardware.
    - Title/menus ran at **`29.4–29.5 FPS`** (`0.85 ms` present), and Central Yharnam (`map/m24`) finished streaming into **live 3D gameplay at `25.8 FPS`** (`presents=1885`, `UWP swap: out=808 in=250 disk=572`, **`corrupt=0`**).
  - All 6 issues identified in the Hardware Test #9/#10 logs (`bb:DrawScan` `+0x270628` sequence-gap crash, `1.28 GiB` host stack commit across 82 OS threads, `GotoPass::MoveOutwardIf` crash on shader `i = 1024`, `174 MiB` `FlushCaches()` memory spike, `.vkcache` `pipelineCacheUUID` validation, and frame-boundary retired dense-arena reclamation) are **fixed, built, and published in `xbox-alpha-1.0.26299.1047` (`a3d39fd`)**, ready for Hardware Test #11 on the console.

---

## 3. Active files

### Build, Packaging, & CI
- `.github/workflows/xbox.yml`: Two-job GitHub Actions pipeline (`dozen` builds `vulkan_dzn.dll` with MSVC `14.44`; `xbox` fetches `dxil.dll` + third-party libs, builds `bloodborne.exe` with `clang-cl`, audits UWP imports, packages/signs `.appx`, and publishes `[release]` pre-releases).
- `xbox/CMakeLists.txt`: UWP target definition (`CMAKE_SYSTEM_NAME=WindowsStore`, `CMAKE_SYSTEM_VERSION=10.0.22621.0`, `/APPCONTAINER`, `/HIGHENTROPYVA:NO`, `/Oy-`, `/STACK:8388608,65536`).
- `xbox/AppxManifest.xml`: Strict UWP AppContainer manifest (`uap:AppSupportsMultipleInstances` omitted, `removableStorage` + PS4 archive `fileTypeAssociation`, zero `runFullTrust` / `broadFileSystemAccess`).
- `xbox/prepared-format.txt`: Monotonic format version (`2`) invalidating stale pre-`01.09`-gated `LocalState\prepared\boot-linked.bin` outputs.
- `xbox/scripts/{fetch-deps.py,boost-headers.py,build-deps.ps1,build.ps1,check-imports.ps1,package.ps1,assets.ps1}`: Dependency fetch/build scripts, PE import verifier (`WindowsApp.lib` + Store CRT allowlist), and `MakeAppx` + `SignTool` packager.

### Xbox Platform, Launcher, Memory, & HLE Runtime
- `xbox/platform/main.cpp`: UWP `CoreApplication` lifecycle, Launcher UI (`Prepare & Launch`, `Precompile shaders`, `Start game`, `Pick USB dump`), `FutureAccessList` USB token persistence, `bbport.ini` preset sync, and `uwp_last_chance_logger` crash breadcrumbs.
- `xbox/platform/prepare_game.cpp` & `xbox/platform/prepare_game.h`: Native C++ PS4 SELF/ELF unpacker, PRX module linker (`libc.prx`, `libSceFios2.prx`), `sysv_abi` trampoline/red-zone patcher (`patches/redzone.json`), `Bloodborne.xml` patch applier, and `01.09` version gate.
- `xbox/platform/guest_memory.inc` & `xbox/platform/guest_swap.inc`: `< 1 TiB` placeholder/view reservation (`VirtualAlloc2FromApp` / `MapViewOfFile3FromApp`), 4 KiB GPU write-tracking protection hooks, `WC_GARLIC`-only sparse swap pager (`LocalState\garlic_swap.bin`), and fault-time pinning of CPU-accessed guest pages.
- `xbox/platform/{input.cpp,audio.cpp,runtime_compat.c,desktop_shims.cpp}`: `Windows.Gaming.Input` gamepad + vibration + virtual keyboard support, `XAudio2` 7.1/stereo output, `< 1 TiB` Win32/UCRT shims, and AppContainer POSIX compatibility.
- `src/probe.c`: ELF image mapper, VEH guest page-fault/GPU write-watch dispatcher, single-winner `fatal_exception` logger, and `< 1 TiB` module/GOT layout.
- `src/{runtime_memory.c,runtime_thread.c,runtime_mutex.c,runtime_rwlock.c,runtime_sema.c,runtime_file.c,runtime_pad.c}`: PS4 Direct/Flexible memory allocator, `< 1 TiB` `low_stack` guest thread trampolines, `GuestMutex` (`0x28` layout), `clang-cl` `__atomic_*` primitives, and case-insensitive UWP/USB file I/O.

### Liverpool GPU, Mesa Dozen Bridge, & Shader Precompiler
- `gpu/CMakeLists.txt` & `gpu/bbgpu.h`: `BB_UWP` build configuration for `bbgpu` static library and C ABI hooks (`bbgpu_uwp_active_precompile_diag`, `bbgpu_uwp_pending_retired_arena_bytes`).
- `gpu/shim/bb_precompile.cpp` & `gpu/shim/bb_precompile.h`: Offline `.shaderbnd.dcx` (`BND4`/`DCX`) archive scanner, `boot-linked.bin`/DLC/mod shader extractor, SPIR-V validator, linked `(VS, PS)` + depth-only `VS` + `RectList`/`QuadList` PSO builder, `precompile_inflight.txt` / `precompile_quarantine_v2.txt` crash recovery, and UUID-validated portable/USB `.vkcache` sync.
- `gpu/shim/{bbgpu.cpp,bbport_overlay.cpp}`: Renderer entrypoints, W^X `64 MiB` SRT walker executable arena (`VirtualProtectFromApp`), controller-driven `L3 + R3` overlay, and gamepad Hunter Name virtual keyboard.
- `xbox/graphics/{present_bridge.cpp,swapchain.cpp,window.cpp,dense_buffers.cpp,descriptor_fallback.cpp}`: D3D12-to-Dozen shared texture swapchain (`frame_index == 0`), deferred `GetDenseArena` `VkBuffer` retirement + pending byte counter, and `maxPerSetDescriptors = 1024` clamping/fallback.
- `gpu/shadps4/video_core/renderer_vulkan/{vk_instance.cpp,vk_draw_prep.h,vk_draw_prep.cpp,vk_pipeline_cache.cpp,vk_pipeline_serialization.cpp,vk_graphics_pipeline.cpp}`: Dozen `5,120 MiB` UMA + `VK_EXT_memory_budget` integration, gap-safe `DrawPreparation` queue + speculative fault recovery (`TryGetParams`), UUID-checked `driver.vkcache` loader, and memory-aware `FlushCaches()`.
- `gpu/shadps4/video_core/{cache_storage.cpp,texture_cache/texture_cache.cpp,amdgpu/regs_shader.h}`: Single-`get_future()` async `DataBase::Flush()`, `AppMemoryUsage`-aware `TextureCache` GC + frame-boundary dense-arena retirement, and non-aborting `TrySearchBinaryInfo` / `TryGetParams`.
- `gpu/shadps4/shader_recompiler/{frontend/control_flow_graph.cpp,frontend/structured_control_flow.cpp,backend/spirv/emit_spirv.cpp,backend/spirv/emit_spirv_context_get_set.cpp,backend/spirv/spirv_emit_context.cpp,ir/passes/flatten_extended_userdata_pass.cpp}`: DFS unreachable-CFG pruning, `GotoPass::MoveOutwardIf` fix + transform/recursion bounds, SPIR-V `Id{0}` / `PerVertex` / `F32[0]` / `OpReturn` fixes, and fault-safe W^X SRT walker signal recovery.

---

## 4. Changes made

1. **GitHub Actions Build & Strict UWP Packaging (`26303d9`..`5e37667`)**:
   - Created `.github/workflows/xbox.yml` to compile Mesa Dozen (`vulkan_dzn.dll`) with MSVC `14.44`, bundle DXC `v1.9.2609` (`dxil.dll`), build `bloodborne.exe` via `clang-cl`, enforce AppContainer PE import rules (`xbox/scripts/check-imports.ps1`), sign `.appx` packages with `xbox/signing/bloodborne_xbox-signing.pfx`, and publish GitHub pre-releases on `[release]` commits.
   - Replaced UWP-blocked file type associations (`.ini`, `.ttf`, `.otf`) in `xbox/AppxManifest.xml` with PS4 Bloodborne archive extensions (`.dcx`, `.bhd`, `.bdt`, `.bnd`, `.chrbnd`, `.partsbnd`, `.mapbnd`, `.shaderbnd`) and persisted USB folder permissions across restarts via `StorageApplicationPermissions::FutureAccessList`.

2. **Native C++ PS4 Dump Preparer & `< 1 TiB` Guest Runtime (`2f694dc`..`05d2d04`)**:
   - Implemented `xbox/platform/prepare_game.cpp` (`prepared-format.txt` = `2`) with `APP_VER = 01.09` enforcement whenever `patches/redzone.json` requires `01.09`, preventing mismatched red-zone/patch offsets on `01.00` base dumps.
   - Restricted all guest-visible pointers, thread stacks (`low_stack`), TLS blocks, `AvPlayer` buffers, and GOT tables below `1 TiB` (`0x10000000000`), fixed `GuestMutex` size/offset (`+0x20`), switched `runtime_mutex.c` atomics to plain `size_t` `__atomic_*` builtins for `clang-cl`, and restricted sparse swap-out (`xbox/platform/guest_swap.inc`) strictly to `WC_GARLIC` pools while pinning any page touched by CPU faults (`corrupt=0`).

3. **Xbox Series S|X `5,120 MiB` UMA, Swapchain, & Dense-Arena Management (`19a8772`, `7197c87`, `9b74bb3`)**:
   - Overrode Dozen's default `1,280 MiB` Series S heap cap to expose the `5,120 MiB` Game Mode budget via `VK_EXT_memory_budget`, matched `NUM_DMA_BUFFERS = BufferCache::DmaViewCount = 176`, and pinned `Swapchain::frame_index = 0` for the single-image D3D12 `PresentBridge`.
   - Deferred superseded `BufferCache::GetDenseArena` `VkBuffer` destruction via `scheduler.DeferOperation`, added `g_uwp_pending_retired_arena_bytes` tracking (`xbox/graphics/dense_buffers.cpp`), called `scheduler.PopPendingOperations()` before allocating replacement arenas, and triggered `scheduler.Finish(); scheduler.PopPendingOperations();` at frame boundaries inside `TextureCache::RunGarbageCollector()` whenever pending retired arenas reach `64 MiB` or `AppMemoryUsage` exceeds pressure thresholds.
   - Changed `xbox/CMakeLists.txt` linker flags from `/STACK:16777216,16777216` to `/STACK:8388608,65536` (`8 MiB` reserve, `64 KiB` initial commit) plus `/Oy-`, reclaiming **`~1.25 GiB` of eagerly committed physical RAM** across Bloodborne's ~82 live OS threads.

4. **Offline `.shaderbnd.dcx` Precompiler & SPIR-V / CFG / SCF Compiler Fixes (`360f2ff`..`9b74bb3`)**:
   - Built `gpu/shim/bb_precompile.cpp` to extract and precompile linked `(VS, PS)`, depth-only `VS` (`ZFormat::Z32Float`, `rasterizationSamples = e1`), and `RectList`/`QuadList` pipelines from all game/DLC/mod `.shaderbnd.dcx` archives and `boot-linked.bin`, writing `LocalState\precompile_inflight.txt` before each compile and quarantining fatal shaders across restarts (`precompile_quarantine_v2.txt`).
   - Fixed `Storage::DataBase::Flush()` double `fence.get_future()` crash (`cache_storage.cpp`), moved the SRT walker JIT into a `64 MiB` W^X arena (`VirtualProtectFromApp` `RW <-> RX`) with AV address verification, capped `MapVsOutputsSafe` to `0..3`, guarded `PerVertex`/`F32[0]`/`BaryCoord*`/`PrimitiveId` `Id{0}` SPIR-V emission paths, replaced single-pass `CFG::RemoveUnreachableBlocks()` with DFS reachability + `num_predecessors` recomputation, and emitted `OpReturn` instead of `OpUnreachable` on `BB_UWP` for Mesa Dozen's `nir_to_dxil` backend.
   - Fixed `GotoPass::MoveOutwardIf` (`structured_control_flow.cpp`) by removing the buggy `if_body.pop_front()` after `if_body.splice(if_body.begin(), body, std::next(goto_stmt), body.end())` (which corrupted the intrusive list sentinel on shader `i = 1024` / `begin=0x27c`), guarding empty `if_body` `If` creation, and bounding `GotoPass` transforms (`65,536`) and `Visit`/`VisitExpr` recursion depth (`512`).

5. **`DrawPreparation` Gap Safety & `VkPipelineCache` Serialization Hardening (`9b74bb3`, `a3d39fd`)**:
   - Fixed the Yharnam `bb:DrawScan` crash (`bloodborne.exe+0x270628`) in `vk_draw_prep.cpp`: replaced raw `submissions[seq - front]` array indexing with binary-searched `FindSubmissionLocked()` and `HasContiguousRangeLocked()`, rebased `tail_regs` / `tail_seq` / `tail_checksum` whenever a memory-pressure-skipped `Build()` leaves a sequence gap, raised the `Build()` skip watermark to `used + 256_MB >= limit` (`4,864 MiB`), and wrapped speculative `RefreshGraphicsStages` (`sel.worker != nullptr`) in `runtime_fault_recover` + `std::optional<Shader::ShaderParams>` / `AmdGpu::TryGetParams()`.
   - Validated `VkPipelineCacheHeaderVersionOne` (`vendorID`, `deviceID`, `pipelineCacheUUID`) on `driver.vkcache` load and portable/USB sync (`512 MiB` cap), freed `driver_cache_data` immediately after `vkCreatePipelineCache`, raised runtime `FlushCaches()` cadence from `32` to `128` new PSOs, skipped transient `174 MiB` `getPipelineCacheData()` dumps during high-memory live gameplay (`!driver_cache_dirty && used + 768_MB >= limit`), and compacted the process heap after cache writes.

---

## 5. Failed attempts

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

## 6. Next steps

1. **Run Hardware Test #11 with `xbox-alpha-1.0.26299.1047`**:
   - Download `BloodborneXbox-1.0.26299.1047.zip` from `https://github.com/youdedend/Test-project/releases/tag/xbox-alpha-1.0.26299.1047` and update `BloodborneLauncher_1.0.26299.1047_x64.appx` via Xbox Device Portal (preserving existing `LocalState` prepared image, `SPRJ0005` save, and `173.7 MiB` `driver.vkcache`).
2. **Verify Post-Fix Telemetry in `loader.log` / `errors.log` / `dozen.log`**:
   - Confirm `presents=0` startup `AppMemoryUsage` drops by `~450–500 MiB` and Central Yharnam (`map/m24`) 3D gameplay `AppMemoryUsage` drops from `4,356–4,583 MiB` down into the **`~3,000–3,350 MiB`** range (`~1.25 GiB` reclaimed from `/STACK:8388608,65536`).
   - Confirm shader `i = 1024` (`begin=0x27c`) compiles cleanly with zero entries in `precompile_quarantine_v2.txt`.
   - Confirm sustained Central Yharnam gameplay, combat, lamp warp to Hunter's Dream, and zone transitions run cleanly past `presents > 10,000` with zero `bb:DrawScan` faults and `corrupt=0`.
3. **Address Any Extended-Play Edge Cases (If Observed in Test #11 Logs)**:
   - Inspect `Thread sample` RVAs (`/Oy-` frame pointers are now enabled in `bloodborne.exe` for multi-frame stack unwinding) and `dozen.log` if any rare runtime shader specialization, cutscene (`AvPlayer`), or boss-arena particle effect exhibits a fault or stutter.
   - If even more Series S RAM headroom is desired for 60 FPS / higher-res modes, `bda_pagetable_buffer` (`128 MiB` in `vk_instance.cpp`) can be lazily committed or sized to active BDA range.
4. **Finalize & Merge PR #10 (`arena/c39edde2-test-project` -> `main`)**:
   - Once Hardware Test #11 confirms sustained, crash-free 3D gameplay across Yharnam and Hunter's Dream, merge PR `#10` (`https://github.com/youdedend/Test-project/pull/10`) into `main` using `gh pr merge 10 --merge`.

---

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
