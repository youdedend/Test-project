// SPDX-License-Identifier: GPL-2.0-or-later
// BbgpuUwpBridge: implements the C ABI in gpu/bbgpu.h on top of D3D12Backend (Game Mode FL 12_0)
// and D3D11FallbackBackend (App Mode FL 10_0), replacing Vulkan (vulkan-1.dll) inside UWP.

#include "D3D12Backend.h"
#if __has_include("../d3d11/D3D11FallbackBackend.h")
#include "../d3d11/D3D11FallbackBackend.h"
#define BBPORT_HAS_D3D11_FALLBACK 1
#endif
#include "../../../gpu/bbgpu.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace {

std::mutex g_gpu_mutex;
void* g_core_window_unknown = nullptr;
uint32_t g_window_width = 1920;
uint32_t g_window_height = 1080;

bbport::uwp::renderer::d3d12::D3D12Backend g_d3d12;
#if defined(BBPORT_HAS_D3D11_FALLBACK)
bbport::uwp::renderer::d3d11::D3D11FallbackBackend g_d3d11;
#endif
#if defined(BBPORT_UWP) && defined(BB_GPU_UWP_ABI_READY)
std::atomic<int> g_active_backend{BB_GPU_BACKEND_NONE};
#else
std::atomic<int> g_active_backend{0};
#endif

} // namespace

extern "C" {

int bbgpu_uwp_probe_d3d12_fl12_0(void) {
    return bbport::uwp::renderer::d3d12::D3D12Backend::ProbeFeatureLevel12_0() ? 1 : 0;
}

#if defined(BBPORT_UWP) && defined(BB_GPU_UWP_ABI_READY)
void bbgpu_uwp_set_core_window(void* core_window_unknown, int32_t width, int32_t height) {
    std::lock_guard<std::mutex> lock(g_gpu_mutex);
    g_core_window_unknown = core_window_unknown;
    if (width > 0) g_window_width = static_cast<uint32_t>(width);
    if (height > 0) g_window_height = static_cast<uint32_t>(height);
}

BbGpuBackend bbgpu_uwp_active_backend(void) {
    return static_cast<BbGpuBackend>(g_active_backend.load(std::memory_order_acquire));
}

void bbgpu_uwp_on_suspending(void) {
    std::lock_guard<std::mutex> lock(g_gpu_mutex);
    const auto active = static_cast<BbGpuBackend>(g_active_backend.load(std::memory_order_acquire));
    if (active == BB_GPU_BACKEND_D3D12) {
        g_d3d12.OnSuspending();
    }
#if defined(BBPORT_HAS_D3D11_FALLBACK)
    else if (active == BB_GPU_BACKEND_D3D11_FALLBACK) {
        g_d3d11.OnSuspending();
    }
#endif
}

void bbgpu_uwp_on_resuming(void) {
    std::lock_guard<std::mutex> lock(g_gpu_mutex);
    const auto active = static_cast<BbGpuBackend>(g_active_backend.load(std::memory_order_acquire));
    if (active == BB_GPU_BACKEND_D3D12) {
        g_d3d12.OnResuming();
    }
#if defined(BBPORT_HAS_D3D11_FALLBACK)
    else if (active == BB_GPU_BACKEND_D3D11_FALLBACK) {
        g_d3d11.OnResuming();
    }
#endif
}

uint64_t bbgpu_uwp_trim_memory(uint64_t target_bytes_to_free) {
    std::lock_guard<std::mutex> lock(g_gpu_mutex);
    if (g_d3d12.IsInitialized()) {
        return g_d3d12.TrimMemory(target_bytes_to_free);
    }
    return 0;
}
#endif

void bbgpu_register_kernel(void) {
    // TODO(port): Link gpu/shim/kernel_equeue.cpp event-queue NID registration into UWP static lib.
}

void bbnet_configure(const char* app0, const char* serial, const char* title, const char* app_ver) {
    // TODO(port): Route shadNet / The Hunter's Dream TCP/TLS sockets over UWP Winsock2 (`internetClient`).
    (void)app0;
    (void)serial;
    (void)title;
    (void)app_ver;
}

void bbnet_set_image(unsigned char* image, uint64_t size) {
    (void)image;
    (void)size;
}

void bbnet_initialize(void) {}

int bbgpu_init(const BbGpuConfig* config) {
    std::lock_guard<std::mutex> lock(g_gpu_mutex);

    const uint32_t req_w = (config && config->width > 0)
        ? static_cast<uint32_t>(config->width)
        : g_window_width;
    const uint32_t req_h = (config && config->height > 0)
        ? static_cast<uint32_t>(config->height)
        : g_window_height;

    bbport::uwp::renderer::d3d12::D3D12InitParams params{};
    params.core_window_unknown = g_core_window_unknown;
    params.width               = req_w;
    params.height              = req_h;
    params.render_preset       = 1; // Quality (720p -> 1080p)
    params.vsync_enabled       = true;
    params.shader_threads      = 2;

    if (g_d3d12.Initialize(params)) {
#if defined(BBPORT_UWP) && defined(BB_GPU_UWP_ABI_READY)
        g_active_backend.store(BB_GPU_BACKEND_D3D12, std::memory_order_release);
#endif
        g_d3d12.RenderFrame(nullptr);
        return 0;
    }

    // D3D12 failed (most commonly because the Xbox Dev Mode package is still set to "App"
    // instead of "Game" in Dev Home). Fall back cleanly to D3D11 Feature Level 10_0.
    std::fprintf(stderr,
                 "[bbgpu-uwp] D3D12 FL 12_0 unavailable; falling back to D3D11 FL 10_0 safe mode.\n");
#if defined(BBPORT_HAS_D3D11_FALLBACK)
    if (g_d3d11.Initialize(g_core_window_unknown, req_w, req_h)) {
#if defined(BBPORT_UWP) && defined(BB_GPU_UWP_ABI_READY)
        g_active_backend.store(BB_GPU_BACKEND_D3D11_FALLBACK, std::memory_order_release);
#endif
        g_d3d11.RenderDiagnosticFrame(nullptr);
        return 0;
    }
#endif

    return -1;
}

uintptr_t bbgpu_resolve(const char* scoped_nid) {
    // TODO(port): Wire libSceGnmDriver & libSceVideoOut HLE NID table (`gpu/shim/gnm_nids.cpp`
    // and `video_out_nids.cpp`) to enqueue PM4 packets into D3D12Backend's command ring.
    (void)scoped_nid;
    return 0;
}

int bbgpu_handle_fault(void* ucontext, void* address) {
    // TODO(port): Connect VirtualProtectFromApp write-watch page faults for GPU BufferCache /
    // TextureCache invalidation (`gpu/shadps4/src/video_core/buffer_cache/`).
    (void)ucontext;
    (void)address;
    return 0;
}

void bbgpu_dump_guest_writes(void* ucontext) {
    (void)ucontext;
}

int bbgpu_text_input_begin(const char* initial_utf8, const char* prompt_utf8) {
    (void)initial_utf8;
    (void)prompt_utf8;
    return 0;
}

int bbgpu_text_input_poll(char* out_utf8, uint64_t size) {
    if (out_utf8 && size > 0) out_utf8[0] = '\0';
    return 2;
}

int bbgpu_overlay_captures_input(void) {
    return 0;
}

int bbgpu_audio_muted(void) {
    return 0;
}

void bbgpu_prompts_configure_pad(const char* const* buttons, int count) {
    (void)buttons;
    (void)count;
}

void bbgpu_prompts_configure(const char* const* inputs, int count) {
    (void)inputs;
    (void)count;
}

void bbgpu_mouse_take(BbMouseInput* out) {
    if (out) {
        std::memset(out, 0, sizeof(*out));
    }
}

void bbgpu_input_configure(int mouse_mode_available, int32_t toggle_scancode, int32_t reload_scancode) {
    (void)mouse_mode_available;
    (void)toggle_scancode;
    (void)reload_scancode;
}

int bbgpu_input_reload_requested(void) {
    return 0;
}

void bbgpu_mouse_camera_configure(int direct, float sensitivity, float scale_x, float scale_y, int sync) {
    (void)direct;
    (void)sensitivity;
    (void)scale_x;
    (void)scale_y;
    (void)sync;
}

unsigned bbgpu_symbol_count(void) {
    return 0;
}

} // extern "C"
