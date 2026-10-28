//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Frame;
}

namespace Vulkan::HostPasses {

class PostProcessingPass {
public:
    /// Push-constant layout, matched field for field by post_process.frag. Every member is 4 bytes
    /// and the flags are `uint` on both sides: a GLSL `bool` in a push-constant block is 4 bytes too,
    /// so the two spellings agree today, but only one of them keeps agreeing when a member is added
    /// next to them. The offsets are asserted below and mirror-checked by graphics_toggles_test.
    struct Settings {
        float gamma = 1.0f;
        u32 hdr = 0;
        u32 srgb_input = 0;
        /// 0 = off and bit-exact. 0..1 = how much of the highlight rolloff is mixed in. A host
        /// post-process step: it cannot remove the game's bloom, it compresses the highlights the
        /// bloom leaves behind, which is what "the lanterns blind me" actually is.
        float bloom = 0.0f;
    };
    // The shader's block is laid out by these offsets, not by this struct's size, so they are
    // asserted here rather than discovered on a console: a mismatch shows up as a picture that
    // ignores the menu, or as the HDR path rendering the wrong colours.
    static_assert(sizeof(Settings) == 16);
    static_assert(offsetof(Settings, gamma) == 0);
    static_assert(offsetof(Settings, hdr) == 4);
    static_assert(offsetof(Settings, srgb_input) == 8);
    static_assert(offsetof(Settings, bloom) == 12);

    void Create(vk::Device device, vk::Format surface_format);

    void Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D input_size,
                Frame& output, Settings settings);

private:
    vk::UniquePipeline pipeline{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniqueSampler sampler{};
};

} // namespace Vulkan::HostPasses
