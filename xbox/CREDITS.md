# Credits

**Bloodborne Launcher** runs Bloodborne (v1.09) natively on **Xbox Series X|S** in Developer Mode
from the player's own decrypted PS4 dump. This package contains **zero** copyrighted game code,
firmware modules, keys, or game assets.

## Built on

- **bloodborne_pc (`bbport`)** by Supermedo and contributors
  (https://github.com/Supermedo/bloodborne_pc): native System V x86-64 PS4 ELF loader, HLE runtime,
  Liverpool GPU integration, UI composition, and in-game settings overlay. GPL-2.0.
- **gow3_xbox** by Cosmo (https://github.com/Cosmo/gow3_xbox) and **SpartanPort** by Felipe Prado
  (https://github.com/FelipePrado0/SpartanPort): Xbox Series X|S UWP AppContainer architecture,
  5,120 MiB unified memory pager, Dozen Vulkan-on-D3D12 integration, and native C++ dump
  preparation design. GPL-2.0.
- **shadPS4** (https://github.com/shadps4-emu/shadPS4): Liverpool GPU emulation, AMD GCN shader
  recompiler, and Vulkan renderer (`gpu/shadps4`). GPL-2.0.
- **KytyPS5 for Xbox** (https://github.com/Cosmo/KytyPS5): Xbox UWP memory paging, overlay, and
  Dev Mode packaging foundations. GPL-2.0.
- **Mesa / Dozen** (`vulkan_dzn.dll`), Vulkan 1.2 on Direct3D 12, built from
  https://github.com/Cosmo/mesa_ai_slop_version. MIT.
- **DirectX Shader Compiler** (`dxil.dll`) by Microsoft. Licenses included in `ThirdParty/`.

## Game patches

- `patches/Bloodborne.xml`: shadPS4/GoldHEN-format patches (`60 FPS++`, resolution scaling,
  visual effect toggles) by **lance**, **illusion**, **manfightdragon**, **MarcosR**, and
  contributors.
- `patches/redzone.json`: System V red-zone-safe trampoline specifications from `bloodborne_pc`.

## Libraries in the package

| Library | License |
|---|---|
| FFmpeg 7.1.1 (static LGPL media decoders) | LGPL-2.1 |
| SDL 3 (static event & mapping helpers) | zlib |
| fmt | MIT |
| Zydis, Zycore | MIT |
| xxHash | BSD-2-Clause |
| Vulkan Memory Allocator | MIT |
| Vulkan-Headers | Apache-2.0 / MIT |
| robin-map | MIT |
| Boost (headers) | BSL-1.0 |
| LibAtrac9 | MIT |
| Dear ImGui | MIT |
| miniz | MIT |
| magic_enum | MIT |
| half | MIT |
| sirit | BSD-3-Clause |
| xbyak | BSD-3-Clause |
| DejaVu fonts | Bitstream Vera / Public Domain |
| winpthreads (MSYS2 UCRT64) | MIT / ZPL |

All third-party license texts are bundled under `ThirdParty/` inside the `.appx` package.

## License

This project is licensed under **GPL-2.0** (`LICENSE.txt`). FFmpeg 7.1.1 is statically linked under
LGPL-2.1 using the exact configuration in `xbox/scripts/build-deps.ps1`, allowing anyone with this
source repository to relink `bloodborne.exe` against a modified FFmpeg build.
