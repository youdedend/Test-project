// SPDX-License-Identifier: GPL-2.0-or-later
// Pure (no Win32, no WinRT) helpers for reading PS4 software-version metadata out of a dump.
//
// They live in their own header so xbox/tests/sfo_version_test.cpp can exercise them on the CI
// build machine: version ordering ("01.09" > "01.00"), CONTENT_ID -> update version
// (UP9000-CUSA03173_00-BLOODBORNE0000000-A0109-V0100 -> 01.09) and APP_TYPE classification
// ("gp" = base game, "patch"/"gd" = update) are the rules every layout decision depends on.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace BbPrepare {

/// Case-insensitive ASCII comparison, the only one the PS4 metadata needs.
inline std::string ToLower(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// True for APP_TYPE values that mark an update/patch container rather than a base game.
/// PSN uses "gp" for a base game and "patch" (some dumps: "gd", "gde") for an update; anything
/// else is treated as a base game so an unknown dump never silently counts as a patch.
inline bool IsPatchAppType(std::string_view app_type) {
    const std::string type = ToLower(app_type);
    return type == "patch" || type == "gd" || type == "gde" || type == "gdp";
}

/// The update version encoded in a CONTENT_ID, e.g.
/// UP9000-CUSA03173_00-BLOODBORNE0000000-A0109-V0100 -> "01.09"
/// The leading letter of the version group is a format marker ("A" = patch); a trailing "V0100"
/// group is the packaging version and must not be mistaken for the app version.
inline std::string UpdateVersionFromContentId(std::string_view content_id) {
    // Walk the hyphen-separated groups from the right: the update group is the last one that
    // carries a four-digit version and is not the "V...." packaging group.
    size_t end = content_id.size();
    while (end > 0) {
        const size_t dash = content_id.rfind('-', end - 1);
        const size_t begin = dash == std::string_view::npos ? 0 : dash + 1;
        const std::string_view group = content_id.substr(begin, end - begin);
        end = dash == std::string_view::npos ? 0 : dash;
        if (group.size() < 5 || group.size() > 5) {
            continue;  // only exactly "A0109"-style groups carry one 4-digit version
        }
        const char marker = static_cast<char>(std::toupper(static_cast<unsigned char>(group[0])));
        if (marker == 'V') {
            continue;  // packaging version, not the application version
        }
        bool digits = true;
        for (size_t i = 1; i < group.size() && digits; ++i) {
            digits = std::isdigit(static_cast<unsigned char>(group[i])) != 0;
        }
        if (!digits) {
            continue;
        }
        // "0109" -> "01.09"
        return std::string{group.substr(1, 2)} + "." + std::string{group.substr(3, 2)};
    }
    return {};
}

/// Numeric compare of two "MM.mm"-style versions ("01.09", "09.00", "1.09"). Missing or malformed
/// versions sort below every real one so an unreadable dump never wins a "highest version" pick.
inline int CompareVersions(std::string_view left, std::string_view right) {
    const auto value = [](std::string_view version) -> int {
        int major = 0;
        int minor = 0;
        size_t i = 0;
        while (i < version.size() && std::isdigit(static_cast<unsigned char>(version[i]))) {
            major = major * 10 + (version[i++] - '0');
        }
        if (i < version.size() && version[i] == '.') {
            ++i;
            while (i < version.size() && std::isdigit(static_cast<unsigned char>(version[i]))) {
                minor = minor * 10 + (version[i++] - '0');
            }
        }
        return major * 1000 + minor;
    };
    const int a = value(left);
    const int b = value(right);
    return a < b ? -1 : (a > b ? 1 : 0);
}

/// The APP_VER the packaged patch set was reverse engineered for (patches/redzone.json records it
/// as "eboot": "Bloodborne 01.09"). This is the *patch set's* requirement, never a guess about a
/// dump: every dump version is still read from that dump's own sce_sys/param.sfo.
inline constexpr std::string_view kPatchTargetVersion = "01.09";

/// True when `version` is exactly the build the packaged patch set targets.
inline bool IsPatchTargetVersion(std::string_view version) {
    return !version.empty() && CompareVersions(version, kPatchTargetVersion) == 0;
}

/// True when `version` is the target build or newer: only there can the red-zone patch set apply.
inline bool ReachesPatchTargetVersion(std::string_view version) {
    return !version.empty() && CompareVersions(version, kPatchTargetVersion) >= 0;
}

} // namespace BbPrepare
