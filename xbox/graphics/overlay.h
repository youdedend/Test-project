// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace Vulkan { class Instance; }
namespace BbUwp::Overlay {
bool Visible();
void Presented();
void Render(const Vulkan::Instance& instance);
} // namespace BbUwp::Overlay
