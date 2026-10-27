// SPDX-License-Identifier: GPL-2.0-or-later
// Minimal `key=value` ini reader/writer for bbport.ini.
//
// The Xbox app owns LocalState\bbport.ini and rewrites exactly one section of it
// (`[dump] patch_path=`, `[dump] use_patch=`) while every other line - the patch settings that the
// Windows launcher wrote and that BuildPatches() reads - has to survive untouched. Parsing and the
// line surgery live here as pure string functions (no file IO, no Win32) so the CI test can cover
// the "my patch folder is forgotten after a restart" round-trip on the build machine.
#pragma once

#include <cctype>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace BbIni {

using Values = std::map<std::string, std::string>;

/// Strips spaces/tabs and a trailing CR from both ends.
inline std::string Trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    while (end > begin &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

/// ASCII lowercase, used for section names and boolean values only.
inline std::string Lower(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = char(c - 'A' + 'a');
        }
    }
    return out;
}

/// Parses `key=value` lines. A `[section]` header qualifies the keys that follow it as
/// `section.key` (`[dump] patch_path=...` -> `dump.patch_path`) while the bare key keeps resolving
/// the way a flat file written by the Windows launcher does, so both layouts work in one file.
/// Later duplicates win, `#`/`;` comments and lines without `=` are ignored.
inline Values Parse(const std::string& text) {
    Values values;
    std::string section;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }
        if (trimmed[0] == '[') {
            const size_t close = trimmed.find(']');
            section = close == std::string::npos
                          ? std::string()
                          : Lower(Trim(trimmed.substr(1, close - 1)));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = Trim(line.substr(0, eq));
        if (key.empty()) {
            continue;
        }
        const std::string value = Trim(line.substr(eq + 1));
        values[key] = value;
        if (!section.empty()) {
            values[section + "." + key] = value;
        }
    }
    return values;
}

/// True for "1"/"on"/"true"/"yes"; false for "0"/"off"/"false"/"no"; `fallback` for a missing or
/// empty value. An unrecognised non-empty value counts as on, the same way the rest of bbport.ini
/// treats flags.
inline bool Flag(const Values& values, const std::string& key, bool fallback) {
    const auto it = values.find(key);
    if (it == values.end()) {
        return fallback;
    }
    const std::string value = Lower(Trim(it->second));
    if (value.empty()) {
        return fallback;
    }
    if (value == "0" || value == "off" || value == "false" || value == "no") {
        return false;
    }
    return true;
}

/// Rewrites `section`/`key` in `text`, keeping every other line (comments, order, unrelated keys)
/// exactly as it was. A missing key is inserted at the end of an existing section; a missing
/// section is appended at the end of the file. Returns the new file text, always LF-terminated.
/// `changed` reports whether the text actually differs from the input.
inline std::string SetValue(const std::string& text, const std::string& section,
                            const std::string& key, const std::string& value, bool* changed) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }

    const std::string header = "[" + Lower(Trim(section)) + "]";
    bool in_section = false;
    bool section_seen = false;
    bool wrote = false;
    bool unchanged = false;
    size_t insert_at = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string trimmed = Trim(lines[i]);
        if (!trimmed.empty() && trimmed[0] == '[') {
            in_section = Lower(trimmed) == header;
            if (in_section) {
                section_seen = true;
                insert_at = i + 1;
            }
            continue;
        }
        if (!in_section) {
            continue;
        }
        const size_t eq = lines[i].find('=');
        if (eq == std::string::npos) {
            if (!trimmed.empty()) {
                insert_at = i + 1;
            }
            continue;
        }
        if (Trim(lines[i].substr(0, eq)) != key) {
            insert_at = i + 1;
            continue;
        }
        if (!wrote) {
            unchanged = Trim(lines[i].substr(eq + 1)) == Trim(value);
            lines[i] = key + "=" + value;
            wrote = true;
            insert_at = i + 1;
        }
    }
    bool modified = true;
    if (!wrote) {
        if (section_seen) {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_at), key + "=" + value);
        } else {
            if (!lines.empty()) {
                lines.emplace_back();
            }
            lines.push_back(header);
            lines.push_back(key + "=" + value);
        }
    } else {
        modified = !unchanged;
    }

    std::string out;
    for (const auto& line : lines) {
        out += line;
        out += '\n';
    }
    if (changed) {
        *changed = modified;
    }
    return out;
}

/// Rewrites a *top-level* `key` (a bare line above the first `[section]`, the layout
/// scripts/patches.py and the Windows launcher write). `SetValue` cannot do this: it looks for a
/// literal `[]` section, whereas a bare key belongs to no section at all. Replacements keep every
/// other line (comments, order, `[dump]`) untouched; duplicates collapse to the one line so a stale
/// copy cannot win, since a later line overrides an earlier one when the file is parsed.
inline std::string SetTopLevel(const std::string& text, const std::string& key,
                               const std::string& value, bool* changed) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    size_t first_section = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string trimmed = Trim(lines[i]);
        if (!trimmed.empty() && trimmed[0] == '[') {
            first_section = i;
            break;
        }
    }
    bool wrote = false;
    bool unchanged = false;
    for (size_t i = 0; i < lines.size();) {
        const std::string trimmed = Trim(lines[i]);
        bool matches = i < first_section && !trimmed.empty() && trimmed[0] != '#' &&
                       trimmed[0] != ';';
        if (matches) {
            const size_t eq = lines[i].find('=');
            matches = eq != std::string::npos && Trim(lines[i].substr(0, eq)) == key;
        }
        if (!matches) {
            ++i;
            continue;
        }
        if (wrote) {
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(i));
            if (i < first_section) {
                --first_section;
            }
            continue;
        }
        unchanged = Trim(lines[i].substr(lines[i].find('=') + 1)) == Trim(value);
        lines[i] = key + "=" + value;
        wrote = true;
        ++i;
    }
    bool modified = true;
    if (!wrote) {
        size_t insert_at = 0;
        for (size_t i = 0; i < first_section; ++i) {
            if (!Trim(lines[i]).empty()) {
                insert_at = i + 1;
            }
        }
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_at), key + "=" + value);
    } else {
        modified = !unchanged;
    }

    std::string out;
    for (const auto& line : lines) {
        out += line;
        out += '\n';
    }
    if (changed) {
        *changed = modified;
    }
    return out;
}

} // namespace BbIni
