// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace BbUwp {
class Presenter {
public:
    Presenter(vk::PhysicalDevice physical, vk::Device device, vk::Queue queue, u32 family,
              u32 width, u32 height);
    ~Presenter();
    vk::Image Image() const;
    vk::ImageView View() const;
    vk::Semaphore Acquired() const;
    vk::Semaphore Ready() const;
    void Resize(u32 width, u32 height);
    bool Acquire();
    bool Present(bool vsync);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

void InitializeDriver();
} // namespace BbUwp
