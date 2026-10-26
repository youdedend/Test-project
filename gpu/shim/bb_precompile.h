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
    u32 total_orbshdr = 0;
    u32 unique_hashes = 0;
    u32 vs_count = 0;
    u32 ps_count = 0;
    u32 cs_count = 0;
    u32 gs_hs_ds_count = 0;
    u32 precompiled_attempted = 0;
    u32 precompiled_ok = 0;
    u32 precompiled_skipped = 0;
};

using ProgressFn = std::function<void(const std::string& label, u32 done, u32 total)>;

/// Imports any portable shader/pipeline cache files from <BB_UWP_DUMP>/shader_cache/<serial>
/// or <LocalState>/shader_cache/<serial> into LocalState/user/cache/<serial> before WarmUp.
u32 SyncPortableCacheToLocal(const ProgressFn& progress = {});

/// Exports newly generated shader/pipeline cache files from LocalState/user/cache/<serial>
/// into <LocalState>/shader_cache/<serial> (and <BB_UWP_DUMP>/shader_cache/<serial> if writable).
u32 ExportLocalCacheToPortable();

/// Streams and decompresses <BB_UWP_DUMP>/dvdroot_ps4/shader/*.shaderbnd.dcx one archive at a
/// time (< 24 MiB transient RAM, freed immediately) to verify OrbShdr binaries on SSD/USB dumps.
ScanSummary ScanGameShaderArchives(const ProgressFn& progress = {});

/// Extracts unique CS/PS/VS OrbShdr programs from Bloodborne's .shaderbnd.dcx archives and
/// ahead-of-time compiles GCN -> SPIR-V -> NIR -> DXIL into Mesa Dozen's VkPipelineCache
/// (driver.vkcache) with < 1 TiB synthetic GNM descriptors and zero permanent PSO memory bloat.
ScanSummary PrecompileGameShaderArchives(Vulkan::Rasterizer& rasterizer,
                                         const ProgressFn& progress = {});

/// Runs portable cache import + PipelineCache::WarmUp with live TV progress before eboot.bin boots.
void WarmUpBeforeBoot(Vulkan::Rasterizer& rasterizer,
                      const std::function<void()>& present_frame);

/// Runs full .shaderbnd.dcx ahead-of-time DXIL precompile + PipelineCache::WarmUp + portable sync.
std::string RunFullVerifyAndWarmUp(Vulkan::Rasterizer& rasterizer,
                                   const std::function<void()>& present_frame);

} // namespace BbPrecompile
