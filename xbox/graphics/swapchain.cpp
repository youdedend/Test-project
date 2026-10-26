// SPDX-License-Identifier: GPL-2.0-or-later
#include "present_bridge.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include <vk_mem_alloc.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Vulkan {
namespace {
bool VsyncEnabled() {
    const char* env = std::getenv("BB_VSYNC");
    return !(env && std::strcmp(env, "0") == 0);
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
    Create(w, h);
}

bool Swapchain::AcquireNextImage() {
    std::scoped_lock lock(Scheduler::submit_mutex);
    return bridge->Acquire();
}

bool Swapchain::Present(vk::Queue) {
    vmaSetCurrentFrameIndex(instance.GetAllocator(), ++frame_index);
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
