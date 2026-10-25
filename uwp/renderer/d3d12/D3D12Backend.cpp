// SPDX-License-Identifier: GPL-2.0-or-later
#include "D3D12Backend.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

namespace bbport::uwp::renderer::d3d12 {

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;

struct D3D12Backend::NativeState {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> graphics_queue;
    ComPtr<IDXGISwapChain3> swap_chain;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12Resource> render_targets[kFrameCount];
    ComPtr<ID3D12CommandAllocator> command_allocators[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> command_list;
    ComPtr<ID3D12RootSignature> liverpool_root_sig;
    ComPtr<ID3D12Fence> frame_fence;
    HANDLE fence_event = nullptr;
    uint64_t fence_values[kFrameCount]{};
    uint32_t frame_index = 0;
    uint32_t rtv_descriptor_size = 0;
    void* cached_core_window = nullptr;
};
#endif

D3D12Backend::D3D12Backend() = default;

D3D12Backend::~D3D12Backend() {
    Shutdown();
}

bool D3D12Backend::ProbeFeatureLevel12_0() {
#if defined(_WIN32)
    ComPtr<ID3D12Device> probe_dev;
    // Request strictly D3D_FEATURE_LEVEL_12_0 (Xbox Dev Mode Game Mode ceiling).
    const HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&probe_dev));
    return SUCCEEDED(hr) && probe_dev != nullptr;
#else
    return false;
#endif
}

bool D3D12Backend::Initialize(const D3D12InitParams& params) {
    if (initialized_) return true;

    width_          = std::max<uint32_t>(params.width, 640u);
    height_         = std::max<uint32_t>(params.height, 360u);
    vsync_enabled_  = params.vsync_enabled;
    shader_threads_ = SpirvToDxilCompiler::ClampXboxCompilerThreads(params.shader_threads);
    scene_scale_    = D3D12RenderPassCache::ComputeXboxSceneScale(width_, height_, params.render_preset);

#if defined(_WIN32)
    native_ = new NativeState();
    native_->cached_core_window = params.core_window_unknown;

    UINT dxgi_flags = 0;
#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debug_ctrl;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_ctrl)))) {
            debug_ctrl->EnableDebugLayer();
            dxgi_flags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif

    if (FAILED(CreateDXGIFactory2(dxgi_flags, IID_PPV_ARGS(&native_->factory)))) {
        Shutdown();
        return false;
    }

    // Check DXGI tearing support for VRR displays; only enable when VSync is explicitly off!
    {
        ComPtr<IDXGIFactory5> factory5;
        BOOL allow = FALSE;
        if (SUCCEEDED(native_->factory.As(&factory5)) &&
            SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                    &allow, sizeof(allow)))) {
            allow_tearing_ = (allow == TRUE);
        }
    }

    // Enumerate hardware adapter supporting strictly D3D_FEATURE_LEVEL_12_0 (Game Mode on Xbox).
    ComPtr<IDXGIAdapter1> chosen_adapter;
    for (UINT idx = 0;
         native_->factory->EnumAdapters1(idx, &chosen_adapter) != DXGI_ERROR_NOT_FOUND;
         ++idx) {
        DXGI_ADAPTER_DESC1 desc{};
        chosen_adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            chosen_adapter.Reset();
            continue;
        }
        if (SUCCEEDED(D3D12CreateDevice(chosen_adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                        IID_PPV_ARGS(&native_->device)))) {
            break;
        }
        chosen_adapter.Reset();
    }

    if (!native_->device) {
        // Fallback default adapter attempt (covers Xbox integrated UMA adapter path).
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
                                     IID_PPV_ARGS(&native_->device)))) {
            Shutdown();
            return false;
        }
    }

    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qdesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (FAILED(native_->device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&native_->graphics_queue)))) {
        Shutdown();
        return false;
    }

    if (params.core_window_unknown) {
        DXGI_SWAP_CHAIN_DESC1 sc_desc{};
        sc_desc.Width       = width_;
        sc_desc.Height      = height_;
        sc_desc.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
        sc_desc.Stereo      = FALSE;
        sc_desc.SampleDesc  = {1, 0};
        sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc_desc.BufferCount = kFrameCount;
        sc_desc.Scaling     = DXGI_SCALING_STRETCH;
        sc_desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sc_desc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
        sc_desc.Flags       = allow_tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(native_->factory->CreateSwapChainForCoreWindow(
                native_->graphics_queue.Get(),
                static_cast<IUnknown*>(params.core_window_unknown),
                &sc_desc,
                nullptr,
                &sc1)) ||
            FAILED(sc1.As(&native_->swap_chain))) {
            Shutdown();
            return false;
        }
        native_->frame_index = native_->swap_chain->GetCurrentBackBufferIndex();
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.NumDescriptors = kFrameCount;
    rtv_desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(native_->device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&native_->rtv_heap)))) {
        Shutdown();
        return false;
    }
    native_->rtv_descriptor_size =
        native_->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    if (native_->swap_chain && !CreateBackbufferRenderTargets()) {
        Shutdown();
        return false;
    }

    for (uint32_t i = 0; i < kFrameCount; ++i) {
        if (FAILED(native_->device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&native_->command_allocators[i])))) {
            Shutdown();
            return false;
        }
    }

    if (FAILED(native_->device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            native_->command_allocators[native_->frame_index].Get(),
            nullptr, IID_PPV_ARGS(&native_->command_list)))) {
        Shutdown();
        return false;
    }
    native_->command_list->Close();

    if (FAILED(native_->device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                            IID_PPV_ARGS(&native_->frame_fence)))) {
        Shutdown();
        return false;
    }
    native_->fence_values[native_->frame_index] = 1;
    native_->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!native_->fence_event) {
        Shutdown();
        return false;
    }

    if (!CreateLiverpoolRootSignature() || !descriptors_.Initialize(native_->device.Get())) {
        Shutdown();
        return false;
    }
#else
    if (!descriptors_.Initialize(nullptr)) {
        return false;
    }
#endif

    // TODO(port): Initialize shadPS4 Liverpool command processor worker pool (`vk_draw_prep.h`
    // Stage A `RegDelta` workers capped to `shader_threads_`) and bind guest DirectMemory aperture
    // `[0x1000000000, ...)` to D3D12 placed/upload buffer mirror (`BufferCache`).

    initialized_ = true;
    return true;
}

#if defined(_WIN32)
bool D3D12Backend::CreateLiverpoolRootSignature() {
    // Root layout matching shadPS4 Liverpool shader stage bindings:
    //   Slot 0: 32x 32-bit root constants (b0, space0) -> PushData user-data SGPRs
    //   Slot 1: Descriptor table t0..t31, space0       -> Liverpool V# buffers & T# textures
    //   Slot 2: Descriptor table u0..u7, space0        -> Compute/GDS/MotionVector UAVs
    //   Slot 3: Descriptor table s0..s15, space0       -> Liverpool S# samplers
    D3D12_DESCRIPTOR_RANGE srv_range{};
    srv_range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors                    = 32;
    srv_range.BaseShaderRegister                = 0;
    srv_range.RegisterSpace                     = 0;
    srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE uav_range{};
    uav_range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uav_range.NumDescriptors                    = 8;
    uav_range.BaseShaderRegister                = 0;
    uav_range.RegisterSpace                     = 0;
    uav_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE samp_range{};
    samp_range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    samp_range.NumDescriptors                    = 16;
    samp_range.BaseShaderRegister                = 0;
    samp_range.RegisterSpace                     = 0;
    samp_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace  = 0;
    params[0].Constants.Num32BitValues = 32;
    params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &srv_range;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges   = &uav_range;
    params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    params[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges   = &samp_range;
    params[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.NumParameters = 4;
    root_desc.pParameters   = params;
    root_desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig_blob;
    ComPtr<ID3DBlob> err_blob;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &sig_blob, &err_blob))) {
        return false;
    }
    return SUCCEEDED(native_->device->CreateRootSignature(
        0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
        IID_PPV_ARGS(&native_->liverpool_root_sig)));
}

bool D3D12Backend::CreateBackbufferRenderTargets() {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = native_->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < kFrameCount; ++i) {
        if (FAILED(native_->swap_chain->GetBuffer(i, IID_PPV_ARGS(&native_->render_targets[i])))) {
            return false;
        }
        native_->device->CreateRenderTargetView(native_->render_targets[i].Get(), nullptr, rtv_handle);
        rtv_handle.ptr += native_->rtv_descriptor_size;
    }
    return true;
}

void D3D12Backend::WaitForGpuIdle() {
    if (!native_ || !native_->graphics_queue || !native_->frame_fence || !native_->fence_event) {
        return;
    }
    const uint64_t fv = native_->fence_values[native_->frame_index];
    if (SUCCEEDED(native_->graphics_queue->Signal(native_->frame_fence.Get(), fv))) {
        if (SUCCEEDED(native_->frame_fence->SetEventOnCompletion(fv, native_->fence_event))) {
            WaitForSingleObjectEx(native_->fence_event, 2000, FALSE);
        }
        native_->fence_values[native_->frame_index] = fv + 1;
    }
}

void D3D12Backend::MoveToNextFrame() {
    const uint64_t current_fence_val = native_->fence_values[native_->frame_index];
    native_->graphics_queue->Signal(native_->frame_fence.Get(), current_fence_val);

    native_->frame_index = native_->swap_chain
        ? native_->swap_chain->GetCurrentBackBufferIndex()
        : ((native_->frame_index + 1) % kFrameCount);

    if (native_->frame_fence->GetCompletedValue() < native_->fence_values[native_->frame_index]) {
        native_->frame_fence->SetEventOnCompletion(
            native_->fence_values[native_->frame_index], native_->fence_event);
        WaitForSingleObjectEx(native_->fence_event, 2000, FALSE);
    }
    native_->fence_values[native_->frame_index] = current_fence_val + 1;
}
#endif

void D3D12Backend::Shutdown() {
#if defined(_WIN32)
    if (native_) {
        WaitForGpuIdle();
        if (native_->fence_event) {
            CloseHandle(native_->fence_event);
            native_->fence_event = nullptr;
        }
        delete native_;
        native_ = nullptr;
    }
#endif
    render_passes_.Clear();
    descriptors_.Shutdown();
    initialized_ = false;
}

bool D3D12Backend::Resize(uint32_t width, uint32_t height) {
    if (!initialized_ || width == 0 || height == 0) return false;
    width_  = width;
    height_ = height;

#if defined(_WIN32)
    if (native_ && native_->swap_chain) {
        WaitForGpuIdle();
        for (uint32_t i = 0; i < kFrameCount; ++i) {
            native_->render_targets[i].Reset();
            native_->fence_values[i] = native_->fence_values[native_->frame_index];
        }
        const UINT flags = allow_tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        if (FAILED(native_->swap_chain->ResizeBuffers(
                kFrameCount, width_, height_, DXGI_FORMAT_R8G8B8A8_UNORM, flags))) {
            return false;
        }
        native_->frame_index = native_->swap_chain->GetCurrentBackBufferIndex();
        return CreateBackbufferRenderTargets();
    }
#endif
    return true;
}

bool D3D12Backend::RenderFrame(const float clear_rgba[4]) {
    if (!initialized_) return false;
    const float default_color[4] = {0.04f, 0.03f, 0.05f, 1.0f}; // Hunter's Dream obsidian tint
    const float* clear = clear_rgba ? clear_rgba : default_color;

#if defined(_WIN32)
    if (native_ && native_->swap_chain) {
        const uint32_t fi = native_->frame_index;
        if (FAILED(native_->command_allocators[fi]->Reset()) ||
            FAILED(native_->command_list->Reset(native_->command_allocators[fi].Get(), nullptr))) {
            return false;
        }

        D3D12_RESOURCE_BARRIER to_rt{};
        to_rt.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_rt.Transition.pResource   = native_->render_targets[fi].Get();
        to_rt.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        to_rt.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        to_rt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        native_->command_list->ResourceBarrier(1, &to_rt);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = native_->rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(fi) * native_->rtv_descriptor_size;
        native_->command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        native_->command_list->ClearRenderTargetView(rtv, clear, 0, nullptr);

        // Bind shader-visible Liverpool SRV/Sampler heaps for draw-call replay.
        ID3D12DescriptorHeap* heaps[2]{};
        uint32_t heap_count = 0;
        if (descriptors_.NativeSrvHeap()) {
            heaps[heap_count++] = static_cast<ID3D12DescriptorHeap*>(descriptors_.NativeSrvHeap());
        }
        if (descriptors_.NativeSamplerHeap()) {
            heaps[heap_count++] = static_cast<ID3D12DescriptorHeap*>(descriptors_.NativeSamplerHeap());
        }
        if (heap_count > 0) {
            native_->command_list->SetDescriptorHeaps(heap_count, heaps);
        }
        native_->command_list->SetGraphicsRootSignature(native_->liverpool_root_sig.Get());

        // TODO(port): Replay shadPS4 Liverpool PM4 command buffer packets here:
        //   1. Drain Stage A `vk_draw_prep.h` `RegDelta` batch and resolve `D3D12RenderPassCache` PSO.
        //   2. Bind memoized `D3D12DescriptorManager::AcquireTextureSetSlice` SRV table at root param 1.
        //   3. Record `DrawIndexedInstanced` / `ExecuteIndirect` (Stage B `vk_draw_pipe.h`).
        //   4. Dispatch synthetic motion vectors (`vk_camera_motion` + `vk_object_motion` compute CS 6.0)
        //      and AMD FidelityFX FSR 2.2 / Native TAA upscale from `scene_scale_.internal_*` to `width_ x height_`.

        D3D12_RESOURCE_BARRIER to_present = to_rt;
        to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        to_present.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        native_->command_list->ResourceBarrier(1, &to_present);

        if (FAILED(native_->command_list->Close())) {
            return false;
        }

        ID3D12CommandList* lists[] = {native_->command_list.Get()};
        native_->graphics_queue->ExecuteCommandLists(1, lists);

        const UINT sync_interval = vsync_enabled_ ? 1u : 0u;
        const UINT present_flags = (!vsync_enabled_ && allow_tearing_) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
        const HRESULT hr = native_->swap_chain->Present(sync_interval, present_flags);
        if (FAILED(hr)) {
            return false;
        }
        MoveToNextFrame();
    }
#else
    (void)clear;
#endif

    ++total_frames_;
    return true;
}

void D3D12Backend::OnSuspending() {
#if defined(_WIN32)
    WaitForGpuIdle();
    ComPtr<IDXGIDevice3> dxgi_dev3;
    if (native_ && native_->device && SUCCEEDED(native_->device.As(&dxgi_dev3))) {
        dxgi_dev3->Trim();
    }
#endif
    // Release cold TextureSetMemo descriptor backing before PLM snapshots process memory.
    descriptors_.EvictColdEntries(128ull * 1024ull * 1024ull, total_frames_, 1);
}

void D3D12Backend::OnResuming() {
#if defined(_WIN32)
    // Swapchain remains valid across standard Xbox PLM suspend/resume when queues were drained.
    (void)native_;
#endif
}

uint64_t D3D12Backend::TrimMemory(uint64_t target_bytes_to_free) {
    return descriptors_.EvictColdEntries(target_bytes_to_free, total_frames_, 3);
}

} // namespace bbport::uwp::renderer::d3d12
