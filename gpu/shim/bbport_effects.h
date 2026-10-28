// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: one table for every user-facing graphics/gameplay toggle.
//
// There used to be three lists of the same toggles: `BbSettings::Effects` (the ini key, the menu
// label and the default), `kEffects` inside xbox/platform/prepare_game.cpp (the console's mapping
// from key to patch name) and `EFFECTS` in scripts/patches.py (the PC launcher's mapping). Three
// copies of one fact is three chances for a toggle to be "in the build" and do nothing: the ini key
// is written, the menu shows it, and one of the two patch compilers does not know it.
//
// This header is the single source of truth for the first two, and `xbox/tests/graphics_toggles_test`
// checks the third against it - every patch name here must exist in patches/Bloodborne.xml and must
// be mapped to the same key in scripts/patches.py.
//
// Conventions:
//   * `key`         the bbport.ini key, also the launcher's key. Never reused.
//   * `label`       the in-game menu text (menu shows the effect, the ini stores the opposite).
//   * `default_on`  the game's own behaviour. "Disable X" toggles default on (X enabled).
//   * `patch_off`   patch applied when the key is 0 (nullptr: nothing to apply).
//   * `patch_on`    patch applied when the key is 1 (nullptr: nothing to apply).
//     A toggle with neither is a runtime feature (a menu item the code acts on directly).
//   * `xbox`        whether the console's menu offers it. Three are desktop-only and stay ini-only
//     there; the reason is in the comment on the row, not in a string comparison somewhere else.
//
// Header-only and dependency-free: the GPU side (gpu/shim/bbport_settings.h), the console's patch
// compiler (xbox/platform/prepare_game.cpp) and the test all include this file.

#pragma once

namespace BbPortEffects {

struct Effect {
    const char* key;
    const char* label;
    bool default_on;
    /// Applied when the ini key is 0 / 1. Null means "no patch for this direction".
    const char* patch_off;
    const char* patch_on;
    /// True for a toggle the code acts on at run time rather than a startup patch.
    bool runtime;
    /// False: keep it out of the console's menu (it still parses from bbport.ini).
    bool xbox;
};

/// Disabling a game effect is a patch at start ("Disable X"); enabling a PC-only effect is a patch
/// too ("Enable SSR"). Cheats and tweaks are always "on" patches: they do not exist in the game.
inline constexpr Effect Effects[] = {
    // --- post-processing ---------------------------------------------------
    // The four the guide calls "core post-processing": a startup patch that removes the game's own
    // pass, so the saving is real and needs a restart.
    {"effect_chromatic_aberration", "Chromatic aberration", true,
     "Disable Chromatic Aberration", nullptr, false, true},
    {"effect_dof", "Depth of field (DoF)", true, "Disable DoF", nullptr, false, true},
    {"effect_motion_blur", "Motion blur", true, "Disable Motion Blur (perf increase)", nullptr, false,
     true},
    // PC-exclusive quality: SSAO and dynamic shadows off by default in the patch set (checked = on),
    // SSR is off in the game and this turns it on. AO strength is not here: the game exposes no
    // scalar for it, only the constructor choice this patch removes.
    {"effect_ssao", "Ambient occlusion (SSAO)", true, "Disable SSAO", nullptr, false, true},
    {"effect_game_aa", "Game anti-aliasing", true, "Disable AA", nullptr, false, true},
    {"effect_dynamic_shadows", "Dynamic shadows", true,
     "Disable Dynamic Light Shadows (perf increase)", nullptr, false, true},
    {"effect_ssr", "Screen-space reflections (SSR)", false, nullptr,
     "Enable Screen Space Reflections (READ NOTE)", false, true},
    // --- intro / debug -----------------------------------------------------
    {"skip_intro", "Skip intro logos and movie", false, nullptr, "Skip Intro", false, true},
    {"debug_camera", "Free camera (Cross + L3)", false, nullptr, "Restore Debug Camera", false, true},
    // The debug menu patch rewrites the game's font paths to files from nexusmods: without them the
    // game crashes on the title screen. It stays ini-only (never applied by the console compiler,
    // never shown in the console menu) exactly as before this table existed.
    {"debug_menu", "Debug menu (requires font files)", false, nullptr, "Restore Debug Menu (READ NOTES)",
     false, false},
    // --- cheats ------------------------------------------------------------
    {"cheat_no_death", "Cheat: never die (minimum 1 HP)", false, nullptr, "Player No Dead (Read note)",
     false, true},
    {"cheat_stealth", "Cheat: enemies cannot see you", false, nullptr, "Player Stealth (Read note)",
     false, true},
    {"cheat_silent", "Cheat: enemies cannot hear you", false, nullptr, "Player Silent (Read note)",
     false, true},
    {"cheat_rally_no_decay", "Cheat: Rally never decays", false, nullptr, "No Rally Decay", false,
     true},
    {"cheat_enemy_control", "Cheat: control targeted enemy (R3 / L3)", false, nullptr,
     "Enemy Control", false, true},
    // --- tweaks ------------------------------------------------------------
    {"tweak_no_rally", "Disable Rally (health recovery)", false, nullptr, "Disable Rally (HP Regain)",
     false, true},
    {"tweak_camera_distance", "Increase camera distance", false, nullptr, "Increased camera distance",
     false, true},
    {"tweak_no_camera_rotation", "Disable automatic camera rotation", false, nullptr,
     "Disable Camera Auto Rotation via Movement", false, true},
    {"tweak_easy_run", "Run with less stick tilt", false, nullptr,
     "Sensitive Analog Input (easier to run)", false, true},
    {"tweak_ragdoll", "Dark Souls-style ragdoll physics", false, nullptr, "DS1-like physics", false,
     true},
    {"tweak_circle_confirm", "Japanese/Asian buttons (Circle confirms)", false, nullptr,
     "Circle Confirm (Japanese/Asian button layout)", false, true},
    // --- runtime features (no patch; the code implements them) ------------
    {"quit_to_desktop", "Quit to Desktop in the title and System menus", true, nullptr, nullptr, true,
     false}, // no desktop on the console
    {"rebirth", "Rebirth at the Doll (5 Insight)", true, nullptr, nullptr, true, false},
};

inline constexpr int EffectCount = int(sizeof(Effects) / sizeof(Effects[0]));

/// Toggles that are not in `Effects[]` because they are neither a patch nor a checkbox: they are
/// numbers the renderer reads every frame. Listed here so the same test can check that each one is
/// still parsed by the ini, saved back, and shown in the menu - the "in the build but not wired"
/// failure mode that a table of checkboxes cannot catch.
struct RuntimeToggle {
    const char* key;      ///< bbport.ini key
    const char* label;    ///< menu text
    const char* field;    ///< the `BbSettings::Values` member that holds it (checked textually)
    const char* note;     ///< what it does, for the menu hint and the log header
};

inline constexpr RuntimeToggle RuntimeToggles[] = {
    {"model_lod", "Model detail (LOD)", "model_lod", "startup patch: -2 highest, 0 the game's, +2 lowest"},
    {"output_res", "Display output", "output_res", "the host's output and UI size"},
    {"preset", "Preset", "preset", "scene resolution relative to the output"},
    {"upscaler", "Upscaler", "upscaler", "off, FSR 3, FSR 4, FSR 4.1.1, TAA or DLSS"},
    {"sharpen", "Sharpening (RCAS)", "sharpen", "the upscaler's sharpening pass"},
    {"sharpness", "Sharpness", "sharpness", "0 silent .. 1 single RCAS pass .. 2 two passes"},
    {"bloom", "Bloom (highlight rolloff)", "bloom",
     "0 off (bit-exact) .. 100: compresses highlights above the knee so lanterns and fires stop "
     "blooming into a white halo; a host post-process step, no extra memory"},
    {"object_motion", "Less ghosting (object motion vectors)", "object_motion",
     "on: characters, cloth and weapons get motion vectors, less ghosting, about 5-10% slower"},
    {"live_resolution", "Live resolution changes", "live_resolution",
     "0 off (startup patch), -1 auto, 1 on: change output and preset without restarting"},
};

inline constexpr int RuntimeToggleCount = int(sizeof(RuntimeToggles) / sizeof(RuntimeToggles[0]));

/// The community port's ini name for the motion-vector toggle is `less_ghosting`; both spellings
/// are accepted so a bbport.ini carried over from a PC install keeps working.
inline constexpr const char* LessGhostingAlias = "less_ghosting";

} // namespace BbPortEffects
