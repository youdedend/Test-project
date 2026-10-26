// SPDX-License-Identifier: GPL-2.0-or-later
// Xbox UWP: resolution scaling uses Bloodborne's startup scene-resolution patch (720p / 900p / 1080p)
// with crisp native 1080p UI, keeping desktop FSR3/FSR4/DLSS/FrameGen providers out of the 5 GiB budget.
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_frame_generation.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "bbport_settings.h"

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
bool FrameGenerator::WantsSceneInputs() const noexcept { return false; }
void FrameGenerator::CaptureInputs(vk::CommandBuffer, vk::ImageView, vk::ImageView, u32, u32,
                                   std::array<float, 2>, const Camera&) {}
void FrameGenerator::CaptureHudless(vk::CommandBuffer, vk::Image, u32, u32, vk::ImageLayout) {}
bool FrameGenerator::Present(Swapchain&, vk::Image, vk::ImageLayout, u32, u32,
                             const OverlayCallback&) {
    return false;
}

struct Fsr4Upscaler::Impl {};
Fsr4Upscaler::Fsr4Upscaler(const Instance&, Scheduler&) {}
Fsr4Upscaler::~Fsr4Upscaler() = default;
bool Fsr4Upscaler::Record(Frame&) { return false; }
void Fsr4Upscaler::ReleaseOutput(vk::CommandBuffer) {}
const char* Fsr4Upscaler::Problem() const noexcept { return "Xbox uses startup resolution patches"; }
bool Fsr4Upscaler::Fatal() const noexcept { return true; }

TemporalUpscaler::TemporalUpscaler(const Instance& i, Scheduler& s, VideoCore::TextureCache& t,
                                   Runtime& r, CameraMotion& c, SceneTargets& targets)
    : instance(i), scheduler(s), texture_cache(t), runtime(r), camera_motion(c),
      scene_targets(targets) {
    BbSettings::ConfigureUpscalerSupport(false, false, "Xbox uses startup resolution patches");
}
TemporalUpscaler::~TemporalUpscaler() = default;
void TemporalUpscaler::OnSceneColor(VideoCore::ImageId) {}
void TemporalUpscaler::OnBlendedSceneDraw() {}
void TemporalUpscaler::OnSceneComposite() {}
void TemporalUpscaler::OnDispatch(u64) {}
bool TemporalUpscaler::OnFrameStart() { return false; }
bool TemporalUpscaler::RasterScaling() const { return false; }
float TemporalUpscaler::SceneMipBias() const { return 0.0f; }
void TemporalUpscaler::OnDraw(u64, VideoCore::ImageId, VideoCore::ImageId, bool) {}
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
