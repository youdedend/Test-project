# Bloodborne on Xbox Dev Mode (`BBPort.UWP`) — Architecture & Status

`BBPort.UWP` adds an in-tree **Universal Windows Platform (`Windows.Universal`, `x64`)** target under [`uwp/`](uwp/) to run `bbport` ([`Supermedo/bloodborne_pc`](https://github.com/Supermedo/bloodborne_pc)) on **Xbox Series X**, **Xbox Series S**, and **Xbox One** consoles in official **Xbox Retail Dev Mode** — without jailbreaks, private GDK SDKs, `runFullTrust`, or `broadFileSystemAccess`.

For full directory trees, PowerShell flags, Linux `ntfs-3g` ACL commands, and host unit test instructions, see **[`uwp/README.md`](uwp/README.md)**.

---

## 1. Current Status (`v0.1.0-uwp-preview` — Draft Architecture)

| Subsystem | Status | Implementation Path(s) |
|---|---|---|
| **UWP `x64` Solution & AppContainer Manifest** | **Implemented** | `uwp/BBPort.UWP.sln`, `uwp/BBPort.UWP/BBPort.UWP.vcxproj`, `uwp/BBPort.UWP/Package.appxmanifest` |
| **AppContainer W^X (`*FromApp`) & 5 GiB Memory Cap** | **Implemented** (`3,328 MiB` `DirectMemory` + `VirtualQuery` walk) | `src/probe.c`, `src/compat_win.c`, `src/runtime_memory.c` |
| **Zero-Picker 10-Foot XAML Launcher & First-Start SFO/ELF Check** | **Implemented** | `uwp/BBPort.UWP/{App.xaml,App.xaml.cs,LauncherPage.xaml,LauncherPage.xaml.cs}`, `uwp/BBPort.UWP/Platform/UwpFileLoader.{h,cpp}` |
| **Sandbox Storage (`LocalFolder` + `TemporaryFolder`) & `userdata0010` Fix** | **Implemented** | `uwp/BBPort.UWP/Platform/StorageLayer.{h,cpp}`, `src/runtime_file.c` |
| **Native Xbox Controller (`Windows.Gaming.Input` + `xinputuap.lib` + DS4 Touchpad Chords)** | **Implemented** | `uwp/BBPort.UWP/Platform/GamepadInput.{h,cpp}`, `uwp/input/DirectInputShim.{h,cpp}`, `src/runtime_pad.c` |
| **Native UWP XAudio2.9 48 kHz Float Stereo Output** | **Implemented** | `uwp/BBPort.UWP/Platform/XAudio2Sink.{h,cpp}`, `src/runtime_audio.c` |
| **PLM `Suspending`/`Resuming` Checkpointing & 4-Tier Memory Eviction** | **Implemented** | `uwp/BBPort.UWP/Platform/LifecycleManager.{h,cpp}`, `uwp/renderer/d3d12/D3D12DescriptorManager.{h,cpp}` |
| **D3D12 FL `12_0` Swapchain/Device + D3D11 FL `10_0` App-Mode Fallback** | **Implemented** | `uwp/renderer/d3d12/D3D12Backend.{h,cpp}`, `uwp/renderer/d3d11/D3D11FallbackBackend.{h,cpp}`, `uwp/renderer/d3d12/BbgpuUwpBridge.cpp` |
| **shadPS4 Liverpool PM4 Draw-Stream Replay & SPIRV-Cross HLSL Codegen** | **Stubbed (`TODO(port)`)** | `uwp/renderer/d3d12/{D3D12Backend,SpirvToDxilCompiler,BbgpuUwpBridge}.cpp` |

---

## 2. Supported Hardware & Xbox Dev Mode Constraints

- **Recommended**: **Xbox Series X** and **Xbox Series S** in **Retail Dev Mode** with Package Type set to **`Game`** (`5,120 MiB` unified memory ceiling, 8-core Zen 2 CPU, Direct3D 12 Feature Level `12_0`, 1080p @ 60 FPS Quality preset `1280x720` internal proxy).
- **Baseline Compatible**: **Xbox One / One S / One X** in **Retail Dev Mode** (`Game` mode, 720p/900p @ 30 FPS target).
- **Hard Platform Constraints Enforced**:
  - `x64` UWP only (`MinVersion="10.0.19041.0"`).
  - **No Vulkan (`vulkan-1.dll`)** and **no D3D9** on Xbox UWP; `BBPORT_UWP` routes `gpu/bbgpu.h` to Direct3D 12 (`D3D_FEATURE_LEVEL_12_0` in Game Mode) with automatic fallback to Direct3D 11 (`D3D_FEATURE_LEVEL_10_0`) if launched in App Mode.
  - **No DirectInput (`dinput8.dll`)**, **no DirectSound (`dsound.dll`)**, and **no XACT**; input uses `Windows.Gaming.Input.Gamepad` + `xinputuap.lib` and audio uses `XAudio2.9`.

---

## 3. Build & Deployment Quickstart

### Prerequisites
1. Windows 10/11 x64 host with **Visual Studio 2022 (`17.8+`)**:
   - Workload: *Universal Windows Platform development* (Windows 10/11 SDK `10.0.22621.0+`, `v143` UWP tools).
   - Component: *C++ Clang tools for Windows* (`clang-cl` for PS4 `__attribute__((sysv_abi))` C11 shims).
2. Xbox console activated via **Xbox Dev Mode Activation** with Remote Access (Device Portal HTTPS port `11443`) enabled.
3. User-supplied, legally dumped **decrypted** Bloodborne (`CUSA03173`) + Update `01.09` (`eboot.bin` ELF64, `sce_sys/param.sfo`, `sce_module/libc.prx`, `sce_module/libSceFios2.prx`, `dvdroot_ps4/`).

### Build, Provision & Deploy
```powershell
# 1. Build signed Release|x64 .msix/.appx package under uwp\AppPackages\:
.\uwp\build.ps1 -Configuration Release -UseClangCL

# 2. Deploy to Xbox Dev Portal and automatically switch App Type -> 'Game' (5 GiB + D3D12 FL 12_0):
.\uwp\deploy.ps1 -XboxIp <XBOX_IP> -PortalUser <USER> -PortalPass <PASS>

# 3. Provision decrypted CUSA03173 v01.09 assets (USB NTFS with *S-1-15-2-1 ACLs, or SFTP port 22):
.\uwp\provision.ps1 -SourceDump "C:\Dumps\CUSA03173" -Mode UsbNtfs -UsbDriveLetter "E:" -RunPrepareBootImage
# OR network fallback:
.\uwp\provision.ps1 -SourceDump "C:\Dumps\CUSA03173" -Mode Sftp -XboxIp <XBOX_IP> -SftpUser DevToolsUser
```

> **USB Data-Integrity Warning**: Always perform a **Full Console Shutdown** (*Settings $\rightarrow$ General $\rightarrow$ Power options $\rightarrow$ Shut down now*) and wait for all LEDs to extinguish before unplugging an NTFS USB drive from an Xbox console.

---

## 4. Controller Support Summary

`GamepadInput.cpp` binds natively to `Windows.Gaming.Input.Gamepad` (with hotplug events and `xinputuap.lib` fallback). All Bloodborne combat actions map 1:1 to standard Xbox buttons, while DualShock 4's split touchpad clicks map cleanly to **View (`⧉`)** = Left Touchpad (*Hunter Gestures*, `x=480, y=471`) and **View (`⧉`) + D-Pad Right** = Right Touchpad (*Personal Effects*, `x=1440, y=471`, suppressing right-hand weapon switch).

---

## 5. Known Limitations

1. **Liverpool PM4 $\rightarrow$ D3D12 Command Replay**: `D3D12Backend` initializes the FL `12_0` device, root signature, descriptor heaps, and CoreWindow swapchain, but `shadPS4` PM4 draw packet recording (`D3D12Backend.cpp:411`) and `SPIRV-Cross` HLSL codegen (`SpirvToDxilCompiler.cpp:162`) remain explicit `TODO(port)` workstreams.
2. **5,120 MiB Dev Mode Memory Ceiling**: Native 4K internal rendering exceeds Dev Mode's 5 GiB cap; use `1080p` output with `Quality` (`1280x720` internal) or `Performance` (`960x540` internal) scene proxies.
3. **Online (`shadNet`)**: `internetClient` is declared in `Package.appxmanifest`, while `bbnet_configure` socket initialization in UWP remains stubbed (`BbgpuUwpBridge.cpp:94`).
