// SPDX-License-Identifier: GPL-2.0-or-later
// Xbox UWP: live host-side 3D scene resolution scaling via SceneTargets (version-agnostic across
// Bloodborne v01.00 and v01.09, keeping Scaleform HUD/menus crisp at 1920x1080), while keeping
// heavy desktop FSR3/FSR4/DLSS/FrameGen providers out of the 5 GiB Game Mode budget.
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_frame_generation.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/ui_composition.h"
#include "bbport_settings.h"
#include <cstdio>
#include <cstdlib>

namespace Vulkan {

// Complete forward-declared Windows helper types owned via std::unique_ptr in vk_frame_generation.h.
class Fg401Native {};
class FusedFgUi {};

struct Dlss::Impl {};
Dlss::Dlss() = default;
Dlss::~Dlss() = default;
Dlss* Dlss::Get() { return nullptr; }
void Dlss::AppendInstanceExtensions(std::vector<const char*>&) {}
void Dlss::AppendDeviceExtensions(vk::Instance, vk::PhysicalDevice, std::vector<const char*>&) {}
void Dlss::Initialize(vk::Instance, vk::PhysicalDevice, vk::Device) {}
void Dlss::Disable(const std::string&) {}
void Dlss::Shutdown() {}
bool Dlss::Available() const { return false; }
std::string Dlss::Problem() const { return "DLSS is not available on Xbox"; }
int Dlss::QualityForScale(float) { return 0; }
bool Dlss::HasFeature(const FeatureDesc&) const { return false; }
bool Dlss::CreateFeature(vk::CommandBuffer, const FeatureDesc&) { return false; }
bool Dlss::Evaluate(vk::CommandBuffer, const Frame&) { return false; }
void Dlss::ReleaseFeature() {}
int Dlss::FrameGenMaxCount() const { return 0; }
bool Dlss::HasFrameGen(const FrameGenDesc&) const { return false; }
bool Dlss::CreateFrameGen(vk::CommandBuffer, const FrameGenDesc&) { return false; }
bool Dlss::EvaluateFrameGen(vk::CommandBuffer, const FrameGenFrame&) { return false; }
void Dlss::ReleaseFrameGen() {}

FrameGenerator::FrameGenerator(const Instance& i, Scheduler& s) : instance(i), scheduler(s) {
    BbSettings::ConfigureFg401Support(false, "Frame generation is disabled on Xbox");
    BbSettings::ConfigureDlssSupport(false, "DLSS is not available on Xbox");
}
FrameGenerator::~FrameGenerator() = default;
bool FrameGenerator::Wanted() const { return false; }
u32 FrameGenerator::WantedCount() const { return 0; }
void FrameGenerator::Skip() {}
void FrameGenerator::Release() {}
vk::ImageView FrameGenerator::BeginCapture(vk::CommandBuffer, vk::ImageView, u32, u32) {
    return {};
}
void FrameGenerator::EndCapture(vk::CommandBuffer, std::array<float, 2>, bool, Camera) {}
void FrameGenerator::CaptureHudless(vk::CommandBuffer, vk::Image, vk::Format, u32, u32) {}
vk::ImageView FrameGenerator::Hudless(vk::Format, vk::Extent2D&) { return {}; }
u32 FrameGenerator::Generate(vk::CommandBuffer&, vk::Image, vk::ImageView, vk::ImageUsageFlags,
                             vk::Image, vk::ImageView, u32, u32, std::span<const Output>) {
    return 0;
}
bool FrameGenerator::OverlapWanted() { return false; }
void FrameGenerator::DrainOverlap() {}
void FrameGenerator::DumpIfRequested(vk::CommandBuffer, vk::Image, u32, u32, const char*) {}

struct Fsr4Upscaler::Impl {};
Fsr4Upscaler::Fsr4Upscaler(const Instance&, Scheduler&) {}
Fsr4Upscaler::~Fsr4Upscaler() = default;
bool Fsr4Upscaler::Record(Frame&) { return false; }
void Fsr4Upscaler::ReleaseOutput(vk::CommandBuffer) {}
const char* Fsr4Upscaler::Problem() const noexcept { return "Xbox uses SceneTargets resolution scaling"; }
bool Fsr4Upscaler::Fatal() const noexcept { return true; }

TemporalUpscaler::TemporalUpscaler(const Instance& i, Scheduler& s, VideoCore::TextureCache& t,
                                   Runtime& r, CameraMotion& c, SceneTargets& targets)
    : instance(i), scheduler(s), texture_cache(t), runtime(r), camera_motion(c),
      scene_targets(targets) {
    BbSettings::ConfigureUpscalerSupport(false, false, "Xbox uses live SceneTargets scaling");
    enabled = true;
    if (const char* fixed = std::getenv("BB_RENDER_RES"); fixed && fixed[0]) {
        int fw = 0, fh = 0;
        if (std::sscanf(fixed, "%dx%d", &fw, &fh) == 2 && fw >= 320 && fh >= 180) {
            scaled_session = true;
            render_width = static_cast<u32>(fw);
            render_height = static_cast<u32>(fh);
        }
    }
    if (scaled_session) {
        BbSettings::Get().active_render_width = static_cast<int>(render_width);
        BbSettings::Get().active_render_height = static_cast<int>(render_height);
        std::printf("Xbox scene resolution: startup-patched %ux%u (HUD 1920x1080)\n",
                    render_width, render_height);
    } else {
        const auto [want_w, want_h] = BbSettings::XboxSceneSize();
        scene_targets.SetSize({static_cast<u32>(want_w), static_cast<u32>(want_h)});
        BbSettings::Get().active_render_width = want_w;
        BbSettings::Get().active_render_height = want_h;
        std::printf("Xbox scene resolution: live %dx%d ready (HUD 1920x1080)\n", want_w, want_h);
    }
}
TemporalUpscaler::~TemporalUpscaler() = default;
void TemporalUpscaler::OnSceneColor(VideoCore::ImageId) {}
void TemporalUpscaler::OnBlendedSceneDraw() {}
void TemporalUpscaler::OnSceneComposite() {}
void TemporalUpscaler::OnDispatch(u64 cs_hash) {
    if (cs_hash == trigger_hash) {
        done_this_frame = true;
    }
}
bool TemporalUpscaler::OnFrameStart() {
    done_this_frame = false;
    if (scaled_session) {
        BbSettings::Get().active_render_width = static_cast<int>(render_width);
        BbSettings::Get().active_render_height = static_cast<int>(render_height);
        return false;
    }
    const auto [want_w, want_h] = BbSettings::XboxSceneSize();
    const bool changed =
        scene_targets.SetSize({static_cast<u32>(want_w), static_cast<u32>(want_h)});
    BbSettings::Get().active_render_width = static_cast<int>(scene_targets.Size().width);
    BbSettings::Get().active_render_height = static_cast<int>(scene_targets.Size().height);
    return changed;
}
bool TemporalUpscaler::RasterScaling() const {
    return !scaled_session && scene_targets.Reduced() && !done_this_frame;
}
float TemporalUpscaler::SceneMipBias() const { return 0.0f; }
void TemporalUpscaler::OnDraw(u64 vs_hash, VideoCore::ImageId, VideoCore::ImageId, bool) {
    if (!scaled_session && (vs_hash == ui_trigger_vs || UiComposition::MovieShader(vs_hash))) {
        done_this_frame = true;
    }
}
void TemporalUpscaler::OnColorTarget(VideoCore::ImageId) {}
bool TemporalUpscaler::RedirectColor(VideoCore::ImageId, const VideoCore::ImageViewInfo&, Target&) {
    return false;
}
bool TemporalUpscaler::RedirectDepth(VideoCore::ImageId, Target&) { return false; }
bool TemporalUpscaler::RedirectSampled(VideoCore::ImageId, const VideoCore::ImageViewInfo&,
                                       vk::ImageView&, vk::ImageLayout&) {
    return false;
}
bool TemporalUpscaler::DisplayOverride(VAddr, Display&) { return false; }

} // namespace Vulkan
