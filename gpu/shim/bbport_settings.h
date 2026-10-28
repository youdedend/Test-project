// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: user settings changed at run time from the in-game menu (bbport_overlay.h) and kept
// in bbport.ini (BB_CONFIG overrides the path). Environment variables override the file at
// start. Readers load the atomics every frame; writers are the menu and Load().

#pragma once

#include <atomic>
#include <utility>

#include "bbport_effects.h"

namespace BbSettings {

enum Upscaler : int { UpscalerOff = 0, UpscalerFsr3 = 1, UpscalerFsr4 = 2, UpscalerFsr411 = 3,
                      UpscalerTaa = 4, UpscalerDlss = 5, UpscalerCount };
/// FSR 4 v07 or FSR 4.1.1: the same inputs, settings and placement in the frame.
inline bool IsFsr4(int upscaler) {
    return upscaler == UpscalerFsr4 || upscaler == UpscalerFsr411;
}
enum Preset : int { NativeAA = 0, Quality, Balanced, Performance, UltraPerformance, PresetCount };
enum XboxScenePreset : int {
    XboxScene360p = 0,
    XboxScene540p = 1,
    XboxScene636p = 2,
    XboxScene720p = 3,
    XboxScene900p = 4,
    XboxScene1080p = 5,
    XboxScene1440p = 6,
    XboxSceneCount = 7
};
inline constexpr int XboxSceneWidths[XboxSceneCount] = {640, 960, 1128, 1280, 1600, 1920, 2560};
inline constexpr int XboxSceneHeights[XboxSceneCount] = {360, 540, 636, 720, 900, 1080, 1440};
inline constexpr int XboxOutputCount = 4; ///< 16:9 console outputs: 720p, 1080p, 1440p, 2160p
enum DebugView : int { DebugNone = 0, DebugReactive = 1, DebugMotion = 2, DebugViewCount };
/// Frame generation (vk_frame_generation.h): frames generated between two of the game's.
enum FrameGen : int { FrameGenOff = 0, FrameGenFsr3 = 1, FrameGenDlss = 2, FrameGenFsr401 = 3, FrameGenCount };
enum Fsr411Backend : int { Fsr411Vulkan = 0, Fsr411Dx12 = 1 };

/// Game effects switched by the community patches at start (patches.py EFFECTS on the PC side,
/// xbox/platform/prepare_game.cpp on the console's). The table itself lives in bbport_effects.h so
/// that the ini, the menu, the console's patch compiler and the test read one list, not three.
using BbPortEffects::Effect;
inline constexpr const Effect* Effects = BbPortEffects::Effects;
inline constexpr int EffectCount = BbPortEffects::EffectCount;
/// Output resolutions: the upscaler's output and the UI host targets. The 16:9 ones change
/// live; the ultrawide ones need the startup patch (patches.py ultrawide_writes, run.py).
inline constexpr int OutputWidths[] = {1280, 1920, 2560, 3840, 2560, 3440, 3840, 5120, 3840, 5120};
inline constexpr int OutputHeights[] = {720, 1080, 1440, 2160, 1080, 1440, 1600, 2160, 1080, 1440};
inline constexpr int OutputCount = 10;
#ifdef BB_UWP
/// A5: 1280x720 (index 0) is the console's default. The reason is measured, not aesthetic: every
/// 1080p-class host target is 2.25x the pixels of the 720p one, and the runs that reached the world
/// load at 5116/5120 MiB did it with 1080p-class targets. The upscaler and frame generation stay off
/// by default for the same reason - each is a full-size extra target - and all three are opt-in
/// (`output_res=1920x1080`, `upscaler=`, `frame_gen=` in bbport.ini).
inline constexpr int OutputDefault = 0; ///< 1280x720 on the console: the headroom A5 asked for
#else
inline constexpr int OutputDefault = 1; ///< 1920x1080, the game's own size
#endif
/// Wider than 16:9: camera and HUD patched at startup.
inline bool Ultrawide(int output) {
    return OutputWidths[output] * 9 > OutputHeights[output] * 16;
}

/// Button prompts (bbport_prompts.cpp): the game's icons switch with the device in use, or stay.
enum Prompts : int {
    PromptsAuto = 0, PromptsPlayStation = 1, PromptsKeyboard = 2, PromptsXbox = 3, PromptsSwitch = 4, PromptsCount
};

/// Field of view (fov.py): the game's camera has 43 degrees.
inline constexpr int FovDefault = 43, FovMin = 30, FovMax = 90;

struct Values {
#ifdef BB_UWP
    std::atomic<int> upscaler{UpscalerOff};
    std::atomic<int> preset{Quality};
#else
    std::atomic<int> upscaler{UpscalerFsr3};
    std::atomic<int> preset{NativeAA};
#endif
    std::atomic<bool> sharpen{true};
    std::atomic<float> sharpness{0.3f};
    std::atomic<bool> jitter{true};
    std::atomic<bool> reactive{false};
#ifdef BB_UWP
    std::atomic<bool> object_motion{false};
#else
    std::atomic<bool> object_motion{true};
#endif
    /// Shared launcher/overlay preferences, applied on the next game start.
    std::atomic<int> fsr411_backend{Fsr411Vulkan};
    std::atomic<bool> object_motion_cache{false};
    std::atomic<float> reactive_scale{1.0f};
    std::atomic<float> reactive_threshold{0.2f};
    std::atomic<float> reactive_max{0.9f};
    std::atomic<int> debug_view{DebugNone};
#ifdef BB_UWP
    std::atomic<bool> show_fps{true};
#else
    std::atomic<bool> show_fps{false};
#endif
    /// Silence the game while its window does not have focus (bbgpu_audio_muted).
    std::atomic<bool> mute_unfocused{false};
    /// Keyboard and mouse button prompts: with the device in use (auto), or always one kind.
#ifdef BB_UWP
    std::atomic<int> button_prompts{PromptsXbox};
#else
    std::atomic<int> button_prompts{PromptsAuto};
#endif
    // FSR 4 checks (menu): the provider's auto exposure, the jitter sign it is given.
    std::atomic<bool> fsr4_auto_exposure{true};
    std::atomic<bool> fsr4_invert_jitter{false};
    std::atomic<int> active_render_width{1920}, active_render_height{1080};
    /// Applied at start (patches.py); the menu shows when a restart is needed.
    std::atomic<bool> effects[EffectCount]{};
    std::atomic<int> model_lod{0}; ///< -2 highest .. 2 lowest, 0 the game's
    std::atomic<int> output_res{OutputDefault}; ///< index into OutputWidths
    /// Ultrawide outputs: the HUD's corner groups, 0 centred in 16:9 .. 100 at the screen edges
    /// (hud_layout.py at start).
    std::atomic<int> hud_position{100};
    /// Vertical field of view in degrees, 43 the game's (run.py, fov.py at start).
    std::atomic<int> fov{FovDefault};
    /// Live resolution and preset changes (run.sh): 0 off by default (startup patch, fastest
    /// on the Steam Deck and older GPUs), -1 auto (strong discrete GPUs), 1 on. On restart.
    std::atomic<int> live_resolution{0};
    /// Why FSR 4 cannot run (assets, device features), or null. Set by the renderer.
    std::atomic<const char*> fsr4_problem{nullptr};
    std::atomic<bool> fsr4_supported{false}, fsr411_supported{false};
    std::atomic<const char*> fsr411_problem{nullptr};
    /// DLSS (bbport_dlss.dll, NVIDIA RTX) is ready, or why not (null before the device exists).
    std::atomic<bool> dlss_supported{false};
    std::atomic<const char*> dlss_problem{nullptr};
    /// Frame generation and the frames per game frame (2x with FSR 3.1, up to 4x with DLSS on
    /// RTX 50). dlssg_max_multiplier: 1 without DLSS Frame Generation.
    std::atomic<int> frame_gen{FrameGenOff};
    /// Session fallback is separate from the saved ML-FG selection.
    std::atomic<int> fg401_fallback{-1};
    int ActiveFrameGen() const {
        const int selected = frame_gen.load();
        const int fallback = fg401_fallback.load();
        return selected == FrameGenFsr401 && fallback >= 0 ? fallback : selected;
    }
    std::atomic<int> frame_gen_multiplier{2};
    /// At most the frames the display shows between two of the game's (an uncapped game then
    /// gets fewer generated frames); off: the multiplier as chosen.
    std::atomic<bool> frame_gen_fit_display{false};
    std::atomic<bool> fg401_supported{false};
    std::atomic<const char*> fg401_problem{nullptr};
    std::atomic<bool> frame_gen_hdr{false};
    std::atomic<int> dlssg_max_multiplier{1};
    std::atomic<const char*> frame_gen_problem{nullptr};
    /// Generated frames per game frame the display has room for now (0: paused), and its
    /// refresh rate (the presenter's measure, for the menu).
    std::atomic<int> frame_gen_now{0};
    std::atomic<int> frame_gen_display_hz{0};
    /// The game's own frame rate (presenter, game frames only), shown beside the total when
    /// frame generation adds frames.
    std::atomic<float> game_fps{0.0f};
    /// The game's frame rate cap frame generation needs (VideoOut applies it), 0 for none: with
    /// FIFO presentation (V-Sync; AMD has no Mailbox) at most refresh / (generated + 1), or each
    /// extra frame waits for the display and generation pauses.
    std::atomic<int> frame_gen_flip_limit{0};

    /// Xbox Series X|S host settings (live scene resolution, V-Sync, frame limiter, gamma, and
    /// 01.09 binary FPS patch selection).
    std::atomic<int> xbox_scene_res{XboxScene720p};
    std::atomic<bool> vsync{true};
    std::atomic<int> fps_cap{60};       ///< 30, 60, 90, 120, or 0 (uncapped)
    std::atomic<float> gamma{1.0f};     ///< post-processing gamma (0.60 .. 1.40)
    /// Bloom reduction in the final post-process pass, 0 (off, bit-exact) .. 100. 0 is the default:
    /// a host effect that changes the picture must be opt-in, and this one costs no memory (it is a
    /// branch and a dot product in a pass that already runs).
    std::atomic<int> bloom{0};
    std::atomic<int> xbox_fps_patch{60}; ///< 01.09 startup patch: 30, 60, 90, or 0 (uncap)
    bool app_ver_is_0109 = false;
    char app_ver_text[16] = "01.00";
    int startup_xbox_fps_patch = 60;

    /// Startup settings for the explicit BB_RENDER_RES compatibility patch only.
    int startup_preset = NativeAA;
    int startup_upscaler = UpscalerFsr3;
    bool startup_object_motion = true;
    int startup_fsr411_backend = Fsr411Vulkan;
    bool startup_object_motion_cache = false;
    bool startup_effects[EffectCount]{};
    int startup_model_lod = 0;
    int startup_output_res = OutputDefault;
    int startup_hud_position = 100;
    int startup_fov = FovDefault;
    int startup_live_resolution = 0;
};

Values& Get();

/// Reads the file, then the environment overrides. Called once at start.
void Load();
/// Checks the loaded choice before the first frame; unsupported FSR 4 uses FSR 3.1.
void ConfigureUpscalerSupport(bool fsr4, bool fsr411, const char* fsr411_problem = nullptr);
/// After device creation: DLSS availability; a DLSS setting falls back to FSR 3.1 without it.
void ConfigureDlssSupport(bool available, const char* problem);
void ConfigureFg401Support(bool available, const char* problem);
void SelectFrameGen(int provider);
void FrameGenFailure(int provider, const char* problem);
/// Startup-patched scene dimensions cannot change until run.sh prepares a new image.
bool FixedRenderSession();
int RenderPreset();
bool ResolutionNeedsRestart();
/// Provider/cache choices are saved now, and retain their startup behavior until restart.
bool FsrSettingsNeedRestart();
/// Writes the file (menu changes).
void Save();

/// Render resolution divisor of a preset (1.0 native, 1.5 quality, ...).
float PresetScale(int preset);
const char* PresetName(int preset);
const char* UpscalerName(int upscaler);
const char* FrameGenName(int frame_gen);
std::pair<int, int> XboxSceneSize();
const char* XboxSceneLabel(int xbox_preset);
bool IsXboxSupportedEffect(int effect_index);

} // namespace BbSettings
