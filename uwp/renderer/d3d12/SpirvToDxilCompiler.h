// SPDX-License-Identifier: GPL-2.0-or-later
// SpirvToDxilCompiler: translates shadPS4 sirit-generated SPIR-V modules into Shader Model 6.0 DXIL
// for Xbox Dev Mode Direct3D 12 (FL 12_0), backed by TemporaryFolder disk caching and a 2-thread
// worker cap to prevent frame-pacing stalls on Xbox Zen 2 cores.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bbport::uwp::renderer::d3d12 {

enum class ShaderStage : uint8_t {
    Vertex = 0,
    Pixel = 1,
    Compute = 2,
    Geometry = 3,
    Hull = 4,
    Domain = 5,
};

struct DxilCompileResult {
    bool ok = false;
    bool loaded_from_disk_cache = false;
    std::vector<uint8_t> dxil_bytecode;
    std::string diagnostic_log;
};

class SpirvToDxilCompiler {
public:
    // Caps shader compiler worker pool on Xbox Series X|S Zen 2 cores to [1, 4] (default 2),
    // applying the gow3_xbox & nex86/dusklight_uwp lesson against 8-thread CPU starvation.
    static uint32_t ClampXboxCompilerThreads(uint32_t requested_threads);

    static std::string BuildCacheKey(uint64_t spirv_hash, ShaderStage stage);

    // Compiles HLSL source directly to SM 6.0 DXIL via dxcompiler.dll (IDxcCompiler3).
    static DxilCompileResult CompileHlslToDxil(const std::string& hlsl_source,
                                               const char* entry_point,
                                               ShaderStage stage,
                                               uint64_t cache_hash = 0);

    // Translates sirit-generated SPIR-V words to HLSL SM 6.0 and compiles to DXIL.
    static DxilCompileResult CompileSpirvToDxil(const uint32_t* spirv_words,
                                                size_t word_count,
                                                ShaderStage stage,
                                                uint64_t shader_hash);
};

} // namespace bbport::uwp::renderer::d3d12
