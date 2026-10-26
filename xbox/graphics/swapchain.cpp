// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_settings.h"
#include "present_bridge.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include <vk_mem_alloc.h>
#include <atomic>
#include <cstdio>

namespace Vulkan {
namespace {
bool VsyncEnabled() {
    return BbSettings::Get().vsync.load(std::memory_order_relaxed);
}
} // namespace

Swapchain::Swapchain(const Instance& i, const Frontend::WindowSDL& w) : instance(i), window(w) {
    surface_format = {vk::Format::eR8G8B8A8Unorm, vk::ColorSpaceKHR::eSrgbNonlinear};
    view_format = surface_format.format;
    present_mode = VsyncEnabled() ? vk::PresentModeKHR::eFifo : vk::PresentModeKHR::eImmediate;
    Create(w.GetWidth(), w.GetHeight());
}

Swapchain::~Swapchain() = default;

void Swapchain::Create(u32 w, u32 h) {
    bridge.reset();
    bridge = std::make_unique<BbUwp::Presenter>(
        instance.GetPhysicalDevice(), instance.GetDevice(), instance.GetPresentQueue(),
        instance.GetPresentQueueFamilyIndex(), w, h);
    width = w;
    height = h;
    extent = {w, h};
    image_count = 1;
    image_index = 0;
    frame_index = 0;
    images = {bridge->Image()};
    images_view = {bridge->View()};
    image_acquired = {bridge->Acquired()};
    present_ready = {bridge->Ready()};
    needs_recreation = false;
}

void Swapchain::Recreate(u32 w, u32 h) {
    if (!bridge) {
        Create(w, h);
        return;
    }
    if (w == width && h == height && !needs_recreation) {
        return;
    }
    std::scoped_lock lock(Scheduler::submit_mutex);
    bridge->Resize(w, h);
    width = w;
    height = h;
    extent = {w, h};
    images = {bridge->Image()};
    images_view = {bridge->View()};
    needs_recreation = false;
}

bool Swapchain::AcquireNextImage() {
    std::scoped_lock lock(Scheduler::submit_mutex);
    return bridge->Acquire();
}

bool Swapchain::Present(vk::Queue) {
    /* Swapchain::frame_index MUST stay 0 on UWP because image_acquired has 1 element.
     * Use an independent monotonic counter to refresh VMA's per-frame budget stats. */
    static std::atomic<u32> vma_frame{0};
    vmaSetCurrentFrameIndex(instance.GetAllocator(),
                            vma_frame.fetch_add(1u, std::memory_order_relaxed) + 1u);
    const bool vsync = VsyncEnabled();
    present_mode = vsync ? vk::PresentModeKHR::eFifo : vk::PresentModeKHR::eImmediate;
    return bridge->Present(vsync);
}

void Swapchain::SetHDR(bool enabled) {
    if (enabled) {
        std::puts("UWP presenter: HDR unavailable; using SDR");
    }
    needs_hdr = false;
}
} // namespace Vulkan
