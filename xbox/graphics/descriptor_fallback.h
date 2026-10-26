// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace BbUwp {
PFN_vkGetInstanceProcAddr WrapUwpGetInstanceProcAddr(PFN_vkGetInstanceProcAddr real_get_proc);
void InstallDescriptorFallback(vk::Device device);
} // namespace BbUwp
