// SPDX-License-Identifier: GPL-2.0-or-later
#include "SpirvToDxilCompiler.h"
#include "../../BBPort.UWP/Platform/StorageLayer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#include <dxcapi.h>
#include <wrl/client.h>
#endif

namespace bbport::uwp::renderer::d3d12 {

namespace {

const wchar_t* StageTargetProfileW(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex:   return L"vs_6_0";
        case ShaderStage::Pixel:    return L"ps_6_0";
        case ShaderStage::Compute:  return L"cs_6_0";
        case ShaderStage::Geometry: return L"gs_6_0";
        case ShaderStage::Hull:     return L"hs_6_0";
        case ShaderStage::Domain:   return L"ds_6_0";
    }
    return L"ps_6_0";
}

const char* StageTag(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex:   return "vs60";
        case ShaderStage::Pixel:    return "ps60";
        case ShaderStage::Compute:  return "cs60";
        case ShaderStage::Geometry: return "gs60";
        case ShaderStage::Hull:     return "hs60";
        case ShaderStage::Domain:   return "ds60";
    }
    return "ps60";
}

} // namespace

uint32_t SpirvToDxilCompiler::ClampXboxCompilerThreads(uint32_t requested_threads) {
    if (requested_threads == 0) return 2u;
    return std::clamp<uint32_t>(requested_threads, 1u, 4u);
}

std::string SpirvToDxilCompiler::BuildCacheKey(uint64_t spirv_hash, ShaderStage stage) {
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%s_%016llx",
                  StageTag(stage), static_cast<unsigned long long>(spirv_hash));
    return buf;
}

DxilCompileResult SpirvToDxilCompiler::CompileHlslToDxil(const std::string& hlsl_source,
                                                         const char* entry_point,
                                                         ShaderStage stage,
                                                         uint64_t cache_hash) {
    DxilCompileResult res{};
    if (hlsl_source.empty() || !entry_point) {
        res.diagnostic_log = "Empty HLSL source or null entry point";
        return res;
    }

    const std::string cache_key = (cache_hash != 0) ? BuildCacheKey(cache_hash, stage) : std::string{};
    if (!cache_key.empty()) {
        std::string cached;
        if (StorageLayer::Instance().ReadShaderCacheBlob(cache_key, &cached) && !cached.empty()) {
            res.ok = true;
            res.loaded_from_disk_cache = true;
            res.dxil_bytecode.assign(cached.begin(), cached.end());
            return res;
        }
    }

#if defined(_WIN32)
    using Microsoft::WRL::ComPtr;
    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    if (FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) ||
        FAILED(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) {
        res.diagnostic_log = "DxcCreateInstance failed (ensure dxcompiler.dll + dxil.dll are packaged in Appx)";
        return res;
    }

    DxcBuffer src_buf{};
    src_buf.Ptr      = hlsl_source.data();
    src_buf.Size     = hlsl_source.size();
    src_buf.Encoding = DXC_CP_UTF8;

    std::wstring entry_w;
    for (const char* p = entry_point; *p; ++p) entry_w.push_back(static_cast<wchar_t>(*p));

    const wchar_t* args[] = {
        L"-E", entry_w.c_str(),
        L"-T", StageTargetProfileW(stage),
        L"-O3",
        L"-Zpr", // Row-major matrices matching shadPS4 SPIR-V layout
    };

    ComPtr<IDxcResult> dxc_res;
    const HRESULT hr = compiler->Compile(
        &src_buf, args, static_cast<UINT32>(sizeof(args) / sizeof(args[0])),
        nullptr, IID_PPV_ARGS(&dxc_res));
    if (FAILED(hr) || !dxc_res) {
        res.diagnostic_log = "IDxcCompiler3::Compile failed";
        return res;
    }

    ComPtr<IDxcBlobUtf8> errors;
    if (SUCCEEDED(dxc_res->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr)) &&
        errors && errors->GetStringLength() > 0) {
        res.diagnostic_log.assign(errors->GetStringPointer(), errors->GetStringLength());
    }

    HRESULT status = E_FAIL;
    dxc_res->GetStatus(&status);
    if (FAILED(status)) {
        return res;
    }

    ComPtr<IDxcBlob> obj;
    if (SUCCEEDED(dxc_res->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr)) &&
        obj && obj->GetBufferSize() > 0) {
        const auto* ptr = static_cast<const uint8_t*>(obj->GetBufferPointer());
        res.dxil_bytecode.assign(ptr, ptr + obj->GetBufferSize());
        res.ok = true;
        if (!cache_key.empty()) {
            StorageLayer::Instance().WriteShaderCacheBlob(
                cache_key, res.dxil_bytecode.data(), res.dxil_bytecode.size());
        }
    }
#else
    (void)StageTargetProfileW;
    res.diagnostic_log = "Host non-Win32 build: DXC runtime compilation requires dxcompiler.dll on Windows/Xbox";
#endif
    return res;
}

DxilCompileResult SpirvToDxilCompiler::CompileSpirvToDxil(const uint32_t* spirv_words,
                                                          size_t word_count,
                                                          ShaderStage stage,
                                                          uint64_t shader_hash) {
    DxilCompileResult res{};
    if (!spirv_words || word_count < 5 || spirv_words[0] != 0x07230203u) {
        res.diagnostic_log = "Invalid SPIR-V header magic (expected 0x07230203)";
        return res;
    }

    // Fast path: check TemporaryFolder DXIL cache before invoking SPIRV-Cross + DXC.
    const std::string cache_key = BuildCacheKey(shader_hash, stage);
    std::string cached;
    if (StorageLayer::Instance().ReadShaderCacheBlob(cache_key, &cached) && !cached.empty()) {
        res.ok = true;
        res.loaded_from_disk_cache = true;
        res.dxil_bytecode.assign(cached.begin(), cached.end());
        return res;
    }

    // TODO(port): Link vendored SPIRV-Cross (`spirv_hlsl.hpp`) against `gpu/shadps4/src/shader_recompiler`
    // output:
    //   1. Instantiate `spirv_cross::CompilerHLSL(spirv_words, word_count)` with `shader_model = 60`.
    //   2. Remap shadPS4 Vulkan push constants (`PushData` in `vk_pipeline_common.h`) to root CBV `b0, space0`.
    //   3. Remap Liverpool `V#` storage buffers -> `t0..t15, space0`, `T#` sampled images -> `t16..t31, space0`,
    //      and `S#` samplers -> `s0..s15, space0` matching `D3D12Backend::CreateLiverpoolRootSignature()`.
    //   4. Pass emitted HLSL string into `CompileHlslToDxil(hlsl, "main", stage, shader_hash)`.
    res.diagnostic_log = "TODO(port): SPIRV-Cross HLSL codegen stubbed pending vendored spirv-cross UWP target";
    return res;
}

} // namespace bbport::uwp::renderer::d3d12
