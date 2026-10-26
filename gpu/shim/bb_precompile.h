// SPDX-License-Identifier: GPL-2.0-or-later
// Xbox UWP shader cache sync, Bloodborne .shaderbnd.dcx OrbShdr verifier, and pre-game warm-up UI.

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
};

using ProgressFn = std::function<void(const std::string& label, u32 done, u32 total)>;

/// Imports any portable shader/pipeline cache files from <BB_UWP_DUMP>/shader_cache/<serial>
/// or <LocalState>/shader_cache/<serial> into LocalState/user/cache/<serial> before WarmUp.
u32 SyncPortableCacheToLocal(const ProgressFn& progress = {});

/// Exports newly generated shader/pipeline cache files from LocalState/user/cache/<serial>
/// into <LocalState>/shader_cache/<serial> so caches can be backed up via Xbox Device Portal.
u32 ExportLocalCacheToPortable();

/// Streams and decompresses <BB_UWP_DUMP>/dvdroot_ps4/shader/*.shaderbnd.dcx one archive at a
/// time (< 24 MiB transient RAM, freed immediately) to verify OrbShdr binaries on SSD/USB dumps.
ScanSummary ScanGameShaderArchives(const ProgressFn& progress = {});

/// Runs portable cache import + PipelineCache::WarmUp with live TV progress before eboot.bin boots.
void WarmUpBeforeBoot(Vulkan::Rasterizer& rasterizer,
                      const std::function<void()>& present_frame);

/// Runs full .shaderbnd.dcx verification + PipelineCache::WarmUp + portable cache export on demand.
std::string RunFullVerifyAndWarmUp(Vulkan::Rasterizer& rasterizer,
                                   const std::function<void()>& present_frame);

} // namespace BbPrecompile
