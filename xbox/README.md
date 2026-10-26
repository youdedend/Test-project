# Bloodborne Launcher — Xbox Series X|S (Developer Mode)

Runs **Bloodborne (v1.09)** natively on **Xbox Series X** and **Xbox Series S** in **Developer Mode**
from your own decrypted PlayStation 4 dump. No copyrighted Sony / FromSoftware files are included
in this repository or in the `.appx` package.

---

## 1. Requirements

1. **Xbox Series X or Xbox Series S** with **Xbox Developer Mode** activated (Xbox One is **not**
   supported because its CPU lacks AVX2/BMI2 and its GPU/RAM budget cannot run Liverpool shaders).
2. Your own **decrypted PS4 Bloodborne dump updated to v1.09** (supported serials: `CUSA03173` GOTY,
   `CUSA00900`, `CUSA00207`, `CUSA00208`, `CUSA00299`, `CUSA01363`, `CUSA03010`, `CUSA03014`,
   `CUSA03023`, `CUSA03179`) containing:
   - `eboot.bin` (decrypted SELF or ELF, v1.09)
   - `sce_module\libc.prx` and `sce_module\libSceFios2.prx` (from your Bloodborne dump)
   - `sce_sys\param.sfo` (`APP_VER = 01.09`)
   - `dvdroot_ps4\` (all game archives, sounds, fonts, movies, and params)

---

## 2. Installing on Xbox Series X|S

1. Download **`BloodborneXbox-<version>.zip`** from **GitHub Actions** (or Releases) and unzip it on
   your PC.
2. Open **Xbox Device Portal** in your browser (`https://<xbox-ip>:11443`).
3. Under **Games & apps → My games & apps → Add**, upload **`BloodborneLauncher_<version>_x64.appx`**
   and add **`Dependencies\Microsoft.VCLibs.x64.14.00.appx`** as its dependency package.
4. **CRITICAL (5 GiB Game Mode RAM):** On the Xbox Dev Home dashboard, highlight **Bloodborne
   Launcher**, press the **View** button (two-squares icon) → **View details**, and set **App type**
   from **App** to **Game**. In **App** mode Xbox caps UWP memory at 1 GiB; in **Game** mode Xbox
   grants the full **5,120 MiB** unified memory budget required by Bloodborne.

---

## 3. Providing Your PS4 Game Files

Choose **either** of these options (the app detects both automatically on startup):

### Option A — USB 3.0 SSD / Flash Drive (NTFS or exFAT)
Copy your decrypted dump folder to:
- `E:\dumps\Bloodborne\` (or `E:\Bloodborne\`)
so `E:\dumps\Bloodborne\eboot.bin` and `E:\dumps\Bloodborne\dvdroot_ps4\` exist, then plug the drive
into the Xbox.

### Option B — Console Internal Dev Mode Storage (Fastest Loading)
Copy your decrypted dump folder anywhere under `D:\DevelopmentFiles`, for example:
- `D:\DevelopmentFiles\Bloodborne\`
via SMB (`\\<xbox-ip>\DevelopmentFiles`) or Xbox Device Portal **File Explorer**.

### Optional — *The Old Hunters* DLC (`SPEXPANSIONDLC03`) & Loose-File Mods
- On GOTY dumps (`CUSA03173`, `CUSA03023`), *The Old Hunters* assets are already included; create an
  empty folder `<dump>\dlc\SPEXPANSIONDLC03` (or `LocalState\dlc\SPEXPANSIONDLC03`) to unlock the
  Eye of a Blood-drunk Hunter entitlement if your dump did not include an `addcont` entitlement dir.
- Loose-file mods can be placed in `<dump>\mods` or `LocalState\mods`.

---

## 4. First Launch & Controls

1. Launch **Bloodborne Launcher** on your Xbox.
2. On first launch, confirm **Prepare & Launch** — the console unpacks `eboot.bin`, links
   `libc.prx` + `libSceFios2.prx`, installs all 607 System V red-zone trampolines, and compiles
   startup patches (`60 FPS++`, `1280x720` scene + crisp native `1920x1080` HUD) into `LocalState\Prepared`
   (~10–20 seconds once; cached on subsequent launches).
3. On the in-app **Bloodborne** launcher screen:
   - Run **Precompile shaders** once (recommended to eliminate first-run DXIL compilation stutter).
   - Select **Start game**!

### Xbox Controller Mapping
- **Left / Right Sticks, D-Pad, LB/RB, LT/RT, A/B/X/Y, Menu (Options):** Standard Soulsborne layout
  with Xbox HUD prompts (`A`/`B`/`X`/`Y`/`LB`/`RB`/`LT`/`RT`) enabled by default.
- **Personal Effects (Right PS4 Touchpad):** Tap **View** (two-squares button), or hold **View + D-Pad Right**.
- **Gestures (Left PS4 Touchpad):** Hold **View + D-Pad Left**.
- **In-Game Settings Overlay (FPS, VSync, Resolution, Effect Toggles, Save & Quit):** Click **L3 + R3**
  together at any time.

---

## 5. Memory management (ported from `gow3_xbox`)

Bloodborne runs inside the **5,120 MiB** Game Mode budget, shared by guest PS4 memory, the sparse
`LocalState` swap file, D3D12 resources and the driver's own allocations. The memory manager is the
one from `cosmo/gow3_xbox`, adapted to Bloodborne's guest runtime:

- **Commit-on-touch guest pages** (`xbox/platform/guest_memory.inc`): guest mappings are placeholders;
  a 16 KiB guest page becomes host memory only when the game touches it, and the GPU page-tracking
  window (256 KiB) is unprotected with one call per 16 KiB page instead of one per 4 KiB page.
- **Sparse pager** (`xbox/platform/guest_swap.inc`): pages are written to a temporary sparse file in
  `LocalState` only when the app is inside the **last 384 MiB** of its budget, and paging stops again
  at **512 MiB** of headroom. Each pass releases 2 MiB and never recycles the shared page-in window
  (`include_views = 0`), so paging cannot churn pages the GPU is reading. The buffer and image
  allocators page memory out on demand when an allocation would cross the limit.
- **Staging rings** (`vk_staging_buffer_pool.cpp`): upload and scratch blocks stay allocated for
  **120 frames** after their last use, so a copy recorded two or three frames ago is never trimmed
  under the GPU, and the kernel does not have to populate a re-created 16 MiB block again.
- **Region-table stall report**: add `BB_IO_STATS=1` to `LocalState\graphics-debug.txt` to log every
  guest map/unmap/protect that held the memory table for **40 ms** or more
  (`Memory lock: <operation> held the region table for <n> ms`).

### Memory contract self-test (on the console)

1. In **Xbox Device Portal → File Explorer**, open `LocalState` of the app.
2. Create `run.txt` whose **first line** is `memory` (a second line is only needed for game modes).
3. Launch the app. The log then prints `UWP memory contract self-test starting`, the swap probe
   numbers, seven `PASS:` lines and `Memory contract self-test returned 0`.

The test walks real guest mappings: lazy zero pages, remap preservation, live aliases, 4 KiB
protection boundaries inside one 16 KiB page, protected host writes, nested region queries with a
queued writer, concurrent commits, disk eviction with checksummed page-in, concurrent readers while
the pager evicts, guest file I/O across a paged range, guest-code pins and released pages. A failure
prints `STOP: swap self-test <n>` (or returns `<n>` from the contract test) so the first broken
invariant is identifiable from the log alone. Delete `run.txt` (or change the first line to `boot`)
to start the game again.
