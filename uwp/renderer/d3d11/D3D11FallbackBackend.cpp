// SPDX-License-Identifier: GPL-2.0-or-later
#include "D3D11FallbackBackend.h"

#include <algorithm>

#if defined(_WIN32)
#include <windows.h>
#include <d3d11_3.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#endif

namespace bbport::uwp::renderer::d3d11 {

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;

struct D3D11FallbackBackend::NativeState {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swap_chain;
    ComPtr<ID3D11RenderTargetView> rtv;
};
#endif

D3D11FallbackBackend::D3D11FallbackBackend() = default;

D3D11FallbackBackend::~D3D11FallbackBackend() {
    Shutdown();
}

bool D3D11FallbackBackend::Initialize(void* core_window_unknown, uint32_t width, uint32_t height) {
    if (initialized_) return true;
    width_  = std::max<uint32_t>(width, 640u);
    height_ = std::max<uint32_t>(height, 360u);

#if defined(_WIN32)
    native_ = new NativeState();

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // Strictly include D3D_FEATURE_LEVEL_10_0 as the guaranteed baseline in Xbox Dev Mode App Mode.
    static constexpr D3D_FEATURE_LEVEL kLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL achieved = D3D_FEATURE_LEVEL_10_0;

    if (FAILED(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            kLevels, static_cast<UINT>(sizeof(kLevels) / sizeof(kLevels[0])),
            D3D11_SDK_VERSION, &native_->device, &achieved, &native_->context))) {
        Shutdown();
        return false;
    }

    if (core_window_unknown) {
        ComPtr<IDXGIDevice2> dxgi_dev;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(native_->device.As(&dxgi_dev)) ||
            FAILED(dxgi_dev->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            Shutdown();
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 sc{};
        sc.Width       = width_;
        sc.Height      = height_;
        sc.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
        sc.SampleDesc  = {1, 0};
        sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc.BufferCount = 2;
        sc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        sc.Scaling     = DXGI_SCALING_STRETCH;
        sc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;

        if (FAILED(factory->CreateSwapChainForCoreWindow(
                native_->device.Get(),
                static_cast<IUnknown*>(core_window_unknown),
                &sc, nullptr, &native_->swap_chain))) {
            Shutdown();
            return false;
        }

        ComPtr<ID3D11Texture2D> backbuffer;
        if (FAILED(native_->swap_chain->GetBuffer(0, IID_PPV_ARGS(&backbuffer))) ||
            FAILED(native_->device->CreateRenderTargetView(backbuffer.Get(), nullptr, &native_->rtv))) {
            Shutdown();
            return false;
        }
    }
#else
    (void)core_window_unknown;
#endif

    initialized_ = true;
    return true;
}

void D3D11FallbackBackend::Shutdown() {
#if defined(_WIN32)
    if (native_) {
        if (native_->context) {
            native_->context->ClearState();
            native_->context->Flush();
        }
        delete native_;
        native_ = nullptr;
    }
#endif
    initialized_ = false;
}

bool D3D11FallbackBackend::RenderDiagnosticFrame(const float clear_rgba[4]) {
    if (!initialized_) return false;
    // Crimson warning tint indicating Xbox Dev Mode is currently in App Mode (FL 10_0) rather than Game Mode (D3D12).
    const float fallback_tint[4] = {0.28f, 0.05f, 0.06f, 1.0f};
    const float* color = clear_rgba ? clear_rgba : fallback_tint;

#if defined(_WIN32)
    if (native_ && native_->swap_chain && native_->rtv) {
        ID3D11RenderTargetView* views[] = {native_->rtv.Get()};
        native_->context->OMSetRenderTargets(1, views, nullptr);
        native_->context->ClearRenderTargetView(native_->rtv.Get(), color);
        return SUCCEEDED(native_->swap_chain->Present(1, 0));
    }
#else
    (void)color;
#endif
    return true;
}

void D3D11FallbackBackend::OnSuspending() {
#if defined(_WIN32)
    if (native_ && native_->device) {
        if (native_->context) {
            native_->context->ClearState();
        }
        ComPtr<IDXGIDevice3> dxgi3;
        if (SUCCEEDED(native_->device.As(&dxgi3))) {
            // Mandatory WACK / Xbox PLM call for Direct3D 11 UWP apps on suspend!
            dxgi3->Trim();
        }
    }
#endif
}

void D3D11FallbackBackend::OnResuming() {
#if defined(_WIN32)
    (void)native_;
#endif
}

} // namespace bbport::uwp::renderer::d3d11
