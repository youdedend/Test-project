// SPDX-License-Identifier: GPL-2.0-or-later
// The text the launcher puts in front of the player and the lines it writes to loader.log.txt.
//
// main.cpp owns the WinRT dialogs and cannot be built on the CI machine, so every sentence shown to
// the player lives here as a pure string builder instead: the CI test can then pin the three
// launcher states (merged, split, base only) and the exact `Prepare:`/`Launch target:` lines, which
// is what someone reading a console log or a bug report is looking at.
#pragma once

#include <string>

#include "prepare_game.h"
#include "sfo_version.h"

namespace BbPanel {

/// `unknown` when a version could not be read; the caller never sees an empty version field.
inline std::string VersionText(const std::string& version) {
    return version.empty() ? std::string("unknown") : version;
}

/// `-` when there is no folder (a base-only launch has no patch path).
inline std::string PathText(const std::string& path) {
    return path.empty() ? std::string("-") : path;
}

/// `<path> - app_ver=01.09 (gp) CUSA03173`
inline std::string DescribeDump(const std::string& path, const BbPrepare::SfoInfo& info) {
    std::string text = path + " - app_ver=" + VersionText(info.Version());
    if (!info.app_type.empty()) {
        text += " (" + info.app_type + ")";
    }
    if (!info.title_id.empty()) {
        text += " " + info.title_id;
    }
    return text;
}

/// What the panel has to say, independent of WinRT: the detected folders, the player's "Use patch"
/// toggle and the version that will actually run.
struct Panel {
    std::string base_path;
    BbPrepare::SfoInfo base;
    std::string patch_path;  ///< empty when no update folder was found
    BbPrepare::SfoInfo patch;
    bool use_patch = true;         ///< `[dump] use_patch`
    std::string effective_ver;     ///< the version that will actually run
    bool can_run_patched = false;  ///< the red-zone patch set can be applied to that version
};

/// The summary above the buttons:
///
///     Base dump:  E:\dumps\Bloodborne - app_ver=01.00 (gp) CUSA03173
///     Patch dump: E:\dumps\Bloodborne-patch - app_ver=01.09 (patch) CUSA03173 (in use)
///     Effective version: 01.09
inline std::string PanelText(const Panel& panel) {
    std::string text = "Base dump:  " + DescribeDump(panel.base_path, panel.base) + "\n";
    if (panel.patch_path.empty()) {
        text += "Patch dump: none found\n";
    } else if (panel.use_patch) {
        text += "Patch dump: " + DescribeDump(panel.patch_path, panel.patch) + " (in use)\n";
    } else {
        text += "Patch dump: " + DescribeDump(panel.patch_path, panel.patch) +
                " (NOT USED - \"Use patch\" is off)\n";
    }
    text += "Effective version: " + VersionText(panel.effective_ver);
    if (!panel.can_run_patched) {
        text += " - patches will be SKIPPED";
    }
    return text;
}

/// The warning under the summary when no update folder was found at all.
inline std::string BaseOnlyBanner(const Panel& panel) {
    std::string text = "! No " + std::string(BbPrepare::kPatchTargetVersion) +
                       " update folder was found.\n"
                       "Without it the red-zone patches are skipped: the game runs unpatched at "
                       "30 FPS and can crash when gameplay starts.\n\n";
    const size_t slash = panel.base_path.find_last_of("\\/");
    const std::string parent =
        slash == std::string::npos ? std::string() : panel.base_path.substr(0, slash);
    text += "Choose the update folder - the one holding eboot.bin and sce_sys\\param.sfo with "
            "APP_TYPE=patch (usually next to the game folder" +
            (parent.empty() ? std::string(")") : ": " + parent + "\\Bloodborne-patch)") +
            ".\n\nThe game folder itself is never modified.";
    return text;
}

/// One line in loader.log.txt that says which folders the run uses and whether it runs patched.
/// `patches` is `required`/`optional`/`skipped` (see EffectivePatchState in main.cpp).
inline std::string LaunchTargetLine(const Panel& panel, const char* patches) {
    return "Launch target: base=" + PathText(panel.base_path) +
           "; patch=" + PathText(panel.patch_path) +
           "; effective=" + VersionText(panel.effective_ver) + "; patches=" + patches;
}

/// `Prepare: base_app_ver=01.00; patch_app_ver=01.09; effective=01.09; patches=required`
/// Empty versions print as `unknown` instead of disappearing from the line.
inline std::string PrepareLine(const std::string& base_ver, const std::string& patch_ver,
                               const std::string& effective_ver, const char* patches) {
    return "Prepare: base_app_ver=" + VersionText(base_ver) +
           "; patch_app_ver=" + VersionText(patch_ver) +
           "; effective=" + VersionText(effective_ver) + "; patches=" + patches;
}

/// Says out loud why the red-zone patch set is not applied, and whether the player agreed to that.
inline std::string SkippedPatchesLine(bool confirmed_by_player) {
    return std::string("Prepare: red-zone patches are skipped, the game runs the unpatched build") +
           (confirmed_by_player ? " (as confirmed by the player)" : "");
}

/// The overlay (and therefore the guest build) changed: the prepared image and the driver pipeline
/// cache describe the old build and are dropped together.
inline std::string OverlayChangedLine(const std::string& previous_digest,
                                      const std::string& new_digest) {
    return "Prepare: overlay changed (digest " +
           (previous_digest.empty() ? std::string("none") : previous_digest) + " -> " +
           (new_digest.empty() ? std::string("none") : new_digest) +
           "); invalidating Prepared and driver.vkcache";
}

} // namespace BbPanel
