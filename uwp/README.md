# Bloodborne — Xbox Dev Mode UWP Port (`BBPort.UWP`)

In-tree Universal Windows Platform (`Windows.Universal`, `x64`) port target for [`Supermedo/bloodborne_pc`](https://github.com/Supermedo/bloodborne_pc), engineered for **Xbox Series X|S** and **Xbox One** consoles running **Xbox Retail Dev Mode** without jailbreaks, private SDKs, `runFullTrust`, or `broadFileSystemAccess`.

---

## 1. Directory Layout (`/uwp/`)

```text
uwp/
├── BBPort.UWP.sln                          # x64 UWP MSBuild solution (Debug|x64, Release|x64)
├── build.ps1                               # vswhere + MSBuild + ClangCL + SHA-256 dev cert packager
├── deploy.ps1                              # Xbox Device Portal REST (+ Game Mode switch) & WinAppDeployCmd
├── provision.ps1                           # USB NTFS (*S-1-15-2-1 ACLs) & SFTP port 22 asset provisioner
├── README.md                               # Detailed UWP developer & deployment guide (this file)
├── LocalAssets/.gitkeep                    # Placeholder documenting LocalState\game\CUSA03173 layout
├── BBPort.UWP/
│   ├── Package.appxmanifest                # Windows.Universal x64 manifest (gamepad, internetClient)
│   ├── BBPort.UWP.vcxproj                  # UWP project (+ in-process ClangCL sysv_abi C11 runtime)
│   ├── BBPort.UWP.vcxproj.filters          # Visual Studio Solution Explorer filter tree
│   ├── App.xaml / App.xaml.cs              # UWP Application shell (XY focus, overscan-free, PLM hooks)
│   ├── LauncherPage.xaml / .cs             # Zero-picker 10-foot TV launcher + SFO/ELF First-Start Check
│   ├── Assets/*.png                        # Appx tile & splash placeholders
│   ├── Signing/.gitkeep                    # Gitignored local temporary signing certificate folder
│   └── Platform/
│       ├── UwpEntryPoint.cpp               # Hardware rating probe + CoreWindow -> bbport_uwp_run_probe
│       ├── GamepadInput.h / .cpp           # Windows.Gaming.Input.Gamepad + xinputuap.lib + DS4 chords
│       ├── StorageLayer.h / .cpp           # LocalFolder, TemporaryFolder DXIL cache, userdata0010 fix
│       ├── UwpFileLoader.h / .cpp          # Binary param.sfo parser, ELF64/SELF verifier, traversal guard
│       ├── LifecycleManager.h / .cpp       # PLM Suspending deferral + 4-tier 5 GiB memory pressure monitor
│       └── XAudio2Sink.h / .cpp            # Native UWP XAudio2.9 48 kHz float stereo voice ring
├── input/
│   └── DirectInputShim.h / .cpp            # Legacy DIJOYSTATE2 -> WGI remapper + SDL2 WinRT GUID audit
├── renderer/
│   ├── d3d12/
│   │   ├── D3D12Backend.h / .cpp           # D3D12 FL 12_0 device, flip-discard swapchain, VSync-guarded VRR
│   │   ├── D3D12DescriptorManager.h / .cpp # TextureSetMemo SRV/Sampler cache + 5 GiB LRU eviction
│   │   ├── D3D12RenderPassCache.h / .cpp   # RenderStateMemo PSO cache + 720p->1080p scene scale proxies
│   │   ├── SpirvToDxilCompiler.h / .cpp    # DXC SM 6.0 DXIL compiler + 2-thread Zen 2 cap + disk cache
│   │   └── BbgpuUwpBridge.cpp              # C ABI implementation of gpu/bbgpu.h under BBPORT_UWP
│   └── d3d11/
│       └── D3D11FallbackBackend.h / .cpp   # D3D11 FL 10_0 App-Mode diagnostic fallback + IDXGIDevice3::Trim
├── scripts/
│   └── reproduce-xbox-uwp-dev.sh           # Replays xbox-uwp-dev onto a user fork of Supermedo/bloodborne_pc
└── tests/
    └── test_uwp_port_core.cpp              # Host C++20 verification suite for UWP platform, input & caches
```

---

## 2. Manifest Capabilities & Why Restricted Capabilities Are Absent

`uwp/BBPort.UWP/Package.appxmanifest` declares only standard, Xbox Dev Mode-compatible capabilities:

- **`<DeviceCapability Name="gamepad" />`**: Mandatory on Xbox UWP; omitting it causes `Windows.Gaming.Input.Gamepad::Gamepads()` to return an empty collection even when controllers are paired.
- **`<Capability Name="internetClient" />`**: Enables outbound TCP/TLS sockets for optional `shadNet` / *The Hunter's Dream* online play.
- **`<uap10:Capability Name="codeGeneration" />`**: Permits `VirtualProtectFromApp` W^X transitions (`PAGE_READWRITE` $\rightarrow$ `PAGE_EXECUTE_READ`) required by `src/probe.c` when patching PS4 `%fs:0x0` TLS loads to Win64 `%gs:[0x1480 + 8*slot]`.
- **Why `runFullTrust` and `broadFileSystemAccess` are strictly absent**:
  - `runFullTrust` (`Windows.FullTrustApplication`) requires desktop Win32 elevation broker components that do not exist on Xbox OS (`Windows.Xbox`) and fail deployment/certification on retail consoles in Dev Mode.
  - `broadFileSystemAccess` prompts desktop Settings consent brokers unavailable on Xbox Game OS and causes erratic `E_ACCESSDENIED` failures across console OS builds. Instead, `StorageLayer` keeps all runtime reads/writes inside `ApplicationData.Current.LocalFolder` and `TemporaryFolder` (plus `D:\DevelopmentFiles` in Dev Mode).

---

## 3. What You Must Supply Locally (Zero Copyrighted Material Included)

This repository contains **zero** PlayStation 4 firmware, keys, executables, or Bloodborne game assets. You must supply your own legally dumped, **decrypted** copy of **Bloodborne (`CUSA03173` recommended) merged with Update `01.09`**:

1. `CUSA03173/eboot.bin` — **Decrypted** PS4 x86-64 ELF64 (`\x7FELF` magic; encrypted PS4 SELF `4F 15 3D 1D` is automatically detected and rejected by `UwpFileLoader` and `provision.ps1`).
2. `CUSA03173/sce_sys/param.sfo` — Binary PSF metadata (`TITLE_ID = CUSA03173`, `APP_VER = 01.09`).
3. `CUSA03173/sce_module/libc.prx` and `CUSA03173/sce_module/libSceFios2.prx` — Decrypted guest runtime modules.
4. `CUSA03173/dvdroot_ps4/` — Full unpacked/mounted game archives (`parts/`, `chr/`, `map/`, `msg/`, `param/`, `sfx/`, `sound/`).
5. *(Optional, generated on host)* `CUSA03173/boot-linked.bin` — Pre-linked `BBPROBE5` boot image produced by `.\uwp\provision.ps1 -RunPrepareBootImage` (`scripts/prepare.py`).

---

## 4. In-Tree Build (`uwp/build.ps1`) & Preserving Desktop Windows

All modifications to shared C/CMake files (`gpu/CMakeLists.txt`, `gpu/bbgpu.h`, `src/compat_win.c`, `src/probe.c`, `src/runtime_audio.c`, `src/runtime_file.c`, `src/runtime_memory.c`, `src/runtime_pad.c`) are strictly isolated behind `#ifdef BBPORT_UWP`. Desktop MSYS2 `CLANG64` builds (`./build.sh`) continue compiling `bb-probe.exe`, `bb-play.exe`, and Vulkan `libbbgpu.a` unchanged.

To build the `x64` UWP `.msix`/`.appx` package on Windows 10/11 (Visual Studio 2022 `17.8+` with **Universal Windows Platform development** + **C++ Clang tools for Windows**):

```powershell
.\uwp\build.ps1 -Configuration Release -UseClangCL
```

---

## 5. Deployment (`uwp/deploy.ps1`) & Game Resource Mode

```powershell
# Deploy via Xbox Device Portal HTTPS API (port 11443) and automatically register PackageType=Game:
.\uwp\deploy.ps1 -XboxIp 192.168.1.42 -PortalUser admin -PortalPass admin

# Or deploy via Windows SDK WinAppDeployCmd.exe using Visual Studio pairing PIN:
.\uwp\deploy.ps1 -XboxIp 192.168.1.42 -Method WinAppDeployCmd -Pin A1B2C3
```

> **CRITICAL — Game Mode vs. App Mode**: In Xbox Dev Home, ensure the package type is set to **`Game`** (not `App`). `App` mode restricts memory to ~1 GiB and blocks `D3D12CreateDevice`, causing `BBPort.UWP` to boot into the crimson `D3D11FallbackBackend` (`FL 10_0`) safe-mode screen. `Game` mode unlocks **5,120 MiB** unified memory and **Direct3D 12 Feature Level `12_0`**.

---

## 6. Asset Provisioning (`uwp/provision.ps1`)

```powershell
# Option A: Windows USB 3.0 NTFS drive (stamps *S-1-15-2-1 ALL APPLICATION PACKAGES ACLs via icacls):
.\uwp\provision.ps1 -SourceDump "C:\Dumps\CUSA03173" -Mode UsbNtfs -UsbDriveLetter "E:" -RunPrepareBootImage

# Option B: SFTP over Gigabit LAN (Port 22, user DevToolsUser + VS Pairing PIN or admin/admin):
.\uwp\provision.ps1 -SourceDump "C:\Dumps\CUSA03173" -Mode Sftp -XboxIp 192.168.1.42 -SftpUser DevToolsUser
```

- **Linux USB Path**: Format with `sudo mkfs.ntfs -Q -L BBPORT_USB /dev/sdX1` and mount via FUSE `sudo ntfs-3g -o permissions /dev/sdX1 /mnt/bbusb` (do **not** rely on kernel `ntfs3` for Xbox AppContainer ACLs), copy `CUSA03173`, stamp `system.ntfs_acl` for `S-1-15-2-1`, `sync`, and `umount`.
- **Mandatory Full Shutdown Before USB Eject**: Always run **Settings $\rightarrow$ General $\rightarrow$ Power options $\rightarrow$ Shut down now** and wait for LEDs to turn completely off before unplugging USB from Xbox.

---

## 7. Known Gaps & Open `TODO(port)` Workstreams

While the UWP packaging, AppContainer `*FromApp` shims, 3,328 MiB memory budget, WGI controller + DirectInput shim, XAudio2.9 sink, `StorageLayer`, `UwpFileLoader`, `LauncherPage`, D3D12 FL `12_0` swapchain/device, `TextureSetMemo` LRU descriptor cache, and D3D11 FL `10_0` fallback are implemented and unit-tested, **end-to-end 3D scene rendering in Yharnam requires wiring the remaining `gpu/shadps4` Liverpool translation hooks**:

1. `uwp/renderer/d3d12/SpirvToDxilCompiler.cpp` (`L162–169`): Link vendored `SPIRV-Cross` (`spirv_hlsl.hpp`) to emit SM 6.0 HLSL from `sirit` SPIR-V before calling `CompileHlslToDxil()`.
2. `uwp/renderer/d3d12/D3D12DescriptorManager.cpp` (`L235`): Copy CPU-staged `TextureCache` SRVs (`ID3D12Device::CopyDescriptorsSimple`) into shader-visible `native_srv_heap_` slices.
3. `uwp/renderer/d3d12/D3D12Backend.cpp` (`L214`, `L411`): Drain `vk_draw_prep.h` Stage A `RegDelta` batches, replay PM4 `DrawIndexedInstanced` / `ExecuteIndirect` calls, and dispatch CS 6.0 motion-vector + FSR 2 / TAA upscaling passes.
4. `uwp/renderer/d3d12/BbgpuUwpBridge.cpp` (`L90`, `L94`, `L152`, `L159`): Wire `kernel_equeue.cpp`, `gnm_nids.cpp` / `video_out_nids.cpp`, `VirtualProtectFromApp` write-watch fault handling for `BufferCache`, and `shadNet` UWP Winsock2 initialization.
