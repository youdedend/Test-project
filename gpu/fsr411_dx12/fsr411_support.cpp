// SPDX-License-Identifier: GPL-2.0-or-later
#include "fsr411_support.h"
#include "ffx_api.h"
#include "ffx_api_loader.h"
#include "ffx_upscale.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstring>
#include <filesystem>
#include <vector>

Fsr411Support ProbeFsr411Dx12(const uint8_t bytes[8]) {
    struct Resources {
        IDXGIFactory4* factory{};
        IDXGIAdapter1* adapter{};
        ID3D12Device* device{};
        HMODULE module{};
        DWORD old_mode{};
        bool changed = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX, &old_mode);
        ~Resources() {
            if (device) device->Release();
            if (adapter) adapter->Release();
            if (factory) factory->Release();
            if (module) FreeLibrary(module);
            if (changed) SetThreadErrorMode(old_mode, nullptr);
        }
    } r;
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), DWORD(path.size())));
    const auto dll = std::filesystem::path(path).parent_path() / L"amd_fidelityfx_upscaler_dx12.dll";
    std::error_code error;
    if (!std::filesystem::is_regular_file(dll, error))
        return {false, "files", "amd_fidelityfx_upscaler_dx12.dll"};
    r.module = LoadLibraryExW(dll.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!r.module)
        return {false, "runtime", "amd_fidelityfx_upscaler_dx12.dll: Windows error " +
            std::to_string(GetLastError()) + "; check runtime dependencies"};
    ffxFunctions ffx{};
    ffxLoadFunctions(&ffx, r.module);
    if (!ffx.Query || !ffx.CreateContext || !ffx.DestroyContext || !ffx.Dispatch)
        return {false, "runtime", "FidelityFX API exports"};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&r.factory))))
        return {false, "driver", "DXGI factory"};
    LUID luid{};
    std::memcpy(&luid, bytes, 8);
    if (!(luid.LowPart || luid.HighPart) ||
        FAILED(r.factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&r.adapter))))
        return {false, "adapter", "Vulkan/DX12 adapter LUID"};
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(r.adapter->GetDesc1(&desc))) return {false, "adapter", "DXGI adapter"};
    if (desc.VendorId != 0x1002 || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        return {false, "hardware", "AMD GPU required"};
    if (FAILED(D3D12CreateDevice(r.adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&r.device))))
        return {false, "driver", "D3D12 feature level 12_0"};
    uint64_t count{};
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = r.device;
    q.outputCount = &count;
    if (ffx.Query(nullptr, &q.header) != FFX_API_RETURN_OK || !count || count > 64)
        return {false, "provider", "AMD FSR 4.1.1 provider enumeration"};
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    q.versionIds = ids.data(); q.versionNames = names.data();
    if (ffx.Query(nullptr, &q.header) != FFX_API_RETURN_OK)
        return {false, "provider", "AMD FSR 4.1.1 provider enumeration"};
    for (const char* name : names)
        if (name && std::strstr(name, "4.1.1") &&
            !std::strstr(name, "i8") && !std::strstr(name, "Int8"))
            return {true, "ready"};
    return {false, "provider", "AMD FSR 4.1.1 provider unavailable on this GPU/driver"};
}
