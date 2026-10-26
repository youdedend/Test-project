// SPDX-License-Identifier: GPL-2.0-or-later
// Xbox UWP shader cache sync, Bloodborne .shaderbnd.dcx OrbShdr precompiler, and pre-game warm-up UI.

#pragma once

#include <functional>
#include <string>
#include "common/types.h"

namespace Vulkan {
class Rasterizer;
}

namespace BbPrecompile {

struct ScanSummary {
    u32 archives_found = 0;
    u32 archives_ok = 0;
    u32 boot_orbshdr = 0;
    u32 total_orbshdr = 0;
    u32 unique_hashes = 0;
    u32 vs_count = 0;
    u32 ps_count = 0;
    u32 cs_count = 0;
    u32 gs_hs_ds_count = 0;
    u32 precompiled_attempted = 0;
    u32 precompiled_ok = 0;
    u32 precompiled_skipped = 0;
    u32 linked_pairs_ok = 0;
    u32 aux_gs_ok = 0;
};

struct ActiveDiagnostic {
    bool active = false;
    u32 index = 0;
    u32 total = 0;
    u32 stage = 0; // 0=CS, 1=PS, 2=VS, 3=VS+PS pair, 4=AuxGS
    u64 hash = 0;
    u64 partner_hash = 0;
};

using ProgressFn = std::function<void(const std::string& label, u32 done, u32 total)>;

/// Returns current precompile item if precompilation is actively compiling a shader on any thread.
ActiveDiagnostic GetActiveShaderDiagnostic() noexcept;

/// Imports any portable shader/pipeline cache files from <BB_UWP_DUMP>/shader_cache/<serial>
/// or <LocalState>/shader_cache/<serial> into LocalState/user/cache/<serial> before WarmUp.
u32 SyncPortableCacheToLocal(const ProgressFn& progress = {});

/// Exports newly generated shader/pipeline cache files from LocalState/user/cache/<serial>
/// into <LocalState>/shader_cache/<serial> (and <BB_UWP_DUMP>/shader_cache/<serial> if writable).
u32 ExportLocalCacheToPortable();

/// Streams and decompresses Bloodborne's .shaderbnd.dcx/.shaderbnd/.gxsh archives (plus boot.bin)
/// one archive at a time (< 24 MiB transient RAM, freed immediately) to verify OrbShdr binaries.
ScanSummary ScanGameShaderArchives(const ProgressFn& progress = {});

/// Extracts unique CS/PS/VS OrbShdr programs + BND4 (VS, PS) pairs + embedded eboot.bin shaders +
/// auxiliary RectList/QuadList GS and ahead-of-time compiles GCN -> SPIR-V -> NIR -> DXIL into
/// Mesa Dozen's VkPipelineCache (driver.vkcache) with < 1 TiB synthetic GNM descriptors.
ScanSummary PrecompileGameShaderArchives(Vulkan::Rasterizer& rasterizer,
                                         const ProgressFn& progress = {});

/// Runs portable cache import + PipelineCache::WarmUp with live TV progress before eboot.bin boots.
void WarmUpBeforeBoot(Vulkan::Rasterizer& rasterizer,
                      const std::function<void()>& present_frame);

/// Runs full .shaderbnd.dcx ahead-of-time DXIL precompile + PipelineCache::WarmUp + portable sync.
std::string RunFullVerifyAndWarmUp(Vulkan::Rasterizer& rasterizer,
                                   const std::function<void()>& present_frame);

} // namespace BbPrecompile
