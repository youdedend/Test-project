// SPDX-License-Identifier: GPL-2.0-or-later
// CI test for gpu/shim/bbport_effects.h and the three places that consume it.
//
// The failure this guards against is not a crash: it is a toggle that exists everywhere a player can
// see it and nowhere the port can act on it. A row in the ini table with no patch name in
// patches/Bloodborne.xml applies nothing; a patch name one character off the XML (`perf increase)`
// vs `perf increase`) throws "patch not found" at start and takes the whole set with it; a runtime
// toggle nobody parses writes `bloom=50` into bbport.ini and shows Off in the menu forever.
//
// So this reads the shipped files - bbport_effects.h, patches/Bloodborne.xml, scripts/patches.py,
// bbport_settings.{h,cpp}, bbport_overlay.cpp, the post-process shader and its pass - and checks that
// they agree with the table. It is a text check on purpose: it is checking the files that ship, not a
// copy of them, and it needs nothing but the standard library.
//
// Run from the repository root (the CI test step runs at the checkout root); the file lookup also
// walks up a few levels so it can be run from xbox/tests.

#include "bbport_effects.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    } else {
        std::printf("ok: %s\n", what.c_str());
    }
}

bool ReadFile(const std::string& path, std::string& out) {
    if (FILE* file = std::fopen(path.c_str(), "rb")) {
        char buffer[8192];
        size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
            out.append(buffer, read);
        }
        std::fclose(file);
        return true;
    }
    return false;
}

/// The repository root, found by walking up from the working directory.
std::string Root() {
    std::string base;
    for (int up = 0; up < 5; ++up) {
        const std::string prefix = base.empty() ? std::string{} : base;
        std::string probe;
        if (ReadFile(prefix + "patches/Bloodborne.xml", probe)) {
            // "." not "": an empty prefix is the answer for the checkout root, and an empty return
            // has to stay the one thing that means "not found".
            return up == 0 ? std::string(".") : prefix;
        }
        base += "../";
    }
    return std::string{};
}

std::string Read(const std::string& root, const std::string& rel) {
    std::string text;
    const bool ok = ReadFile(root + "/" + rel, text);
    Check(ok, "can read " + rel);
    return text;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

/// The `<Metadata ... Name="...">` values of a patches file, as quoted strings.
std::vector<std::string> PatchNames(const std::string& xml) {
    std::vector<std::string> names;
    size_t at = 0;
    while ((at = xml.find("Name=\"", at)) != std::string::npos) {
        at += 6;
        const size_t end = xml.find('"', at);
        if (end == std::string::npos) {
            break;
        }
        names.push_back(xml.substr(at, end - at));
        at = end;
    }
    return names;
}

/// The single-line `'key':('off','on'),` entries of scripts/patches.py, keyed by the ini key. The
/// value is the entry's text, so a test can ask "does this line name this patch?".
std::vector<std::pair<std::string, std::string>> PythonEffects(const std::string& py) {
    std::vector<std::pair<std::string, std::string>> entries;
    size_t at = py.find("EFFECTS={");
    if (at == std::string::npos) {
        return entries;
    }
    size_t end = py.find("\n}", at);
    if (end == std::string::npos) {
        end = py.size();
    }
    const std::string block = py.substr(at, end - at);
    size_t line = 0;
    while (line < block.size()) {
        size_t eol = block.find('\n', line);
        if (eol == std::string::npos) {
            eol = block.size();
        }
        std::string text = block.substr(line, eol - line);
        line = eol + 1;
        if (text.find('\'') == std::string::npos) {
            continue;
        }
        const size_t colon = text.find("':(");
        if (colon == std::string::npos) {
            continue;
        }
        const size_t open = text.find('\'', 0);
        entries.emplace_back(text.substr(open + 1, colon - open - 1), text);
    }
    return entries;
}

bool Has(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

const char* const kNull = nullptr;

std::string Str(const char* text) {
    return text ? std::string(text) : std::string();
}

} // namespace

int main() {
    const std::string root = Root();
    if (root.empty()) {
        std::printf("FATAL: cannot find the repository root (run from the checkout)\n");
        return 1;
    }

    const std::string xml = Read(root, "patches/Bloodborne.xml");
    const std::string py = Read(root, "scripts/patches.py");
    const std::string settings_h = Read(root, "gpu/shim/bbport_settings.h");
    const std::string settings_cpp = Read(root, "gpu/shim/bbport_settings.cpp");
    const std::string overlay = Read(root, "gpu/shim/bbport_overlay.cpp");
    const std::string shader = Read(root, "gpu/shadps4/video_core/host_shaders/post_process.frag");
    const std::string pp_h =
        Read(root, "gpu/shadps4/video_core/renderer_vulkan/host_passes/pp_pass.h");
    const std::string presenter = Read(root, "gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp");

    const std::vector<std::string> patch_names = PatchNames(xml);
    const auto python_effects = PythonEffects(py);
    Check(patch_names.size() > 50, "the patches file has its Metadata names");
    Check(!python_effects.empty(), "scripts/patches.py has an EFFECTS table");

    // --- the table itself ---------------------------------------------------
    for (int e = 0; e < BbPortEffects::EffectCount; ++e) {
        const auto& row = BbPortEffects::Effects[e];
        const std::string key = Str(row.key);
        const std::string label = Str(row.label);
        Check(!key.empty(), "effect " + std::to_string(e) + " has a key");
        Check(!label.empty(), "effect " + key + " has a menu label");
        Check(row.patch_off != kNull || row.patch_on != kNull || row.runtime,
              "effect " + key + " either patches something or is marked runtime");
        for (int other = e + 1; other < BbPortEffects::EffectCount; ++other) {
            if (key == Str(BbPortEffects::Effects[other].key)) {
                Check(false, "effect key '" + key + "' is used twice");
            }
        }
    }

    // --- every patch name exists in the shipped patch set --------------------
    for (int e = 0; e < BbPortEffects::EffectCount; ++e) {
        const auto& row = BbPortEffects::Effects[e];
        for (const char* name : {row.patch_off, row.patch_on}) {
            if (!name) {
                continue;
            }
            Check(Has(patch_names, name),
                  std::string("patch '") + name + "' (" + Str(row.key) + ") exists in Bloodborne.xml");
        }
    }

    // --- and the desktop patch compiler maps the same names to the same key --
    for (int e = 0; e < BbPortEffects::EffectCount; ++e) {
        const auto& row = BbPortEffects::Effects[e];
        if (!row.patch_off && !row.patch_on) {
            continue; // runtime feature: no patch, so nothing for patches.py to map
        }
        const std::string key = Str(row.key);
        const auto found = std::find_if(python_effects.begin(), python_effects.end(),
                                        [&](const auto& entry) { return entry.first == key; });
        Check(found != python_effects.end(), "scripts/patches.py maps '" + key + "'");
        if (found != python_effects.end()) {
            for (const char* name : {row.patch_off, row.patch_on}) {
                if (name) {
                    Check(Contains(found->second, std::string("'") + name + "'"),
                          "scripts/patches.py maps '" + key + "' to '" + name + "'");
                }
            }
        }
    }

    // --- runtime toggles are parsed, stored and shown ------------------------
    for (int t = 0; t < BbPortEffects::RuntimeToggleCount; ++t) {
        const auto& row = BbPortEffects::RuntimeToggles[t];
        const std::string key = Str(row.key);
        const std::string field = Str(row.field);
        Check(!key.empty() && !field.empty(), "runtime toggle " + std::to_string(t) + " is named");
        // Parsed: a `key ==` comparison in the ini reader on the GPU side...
        Check(Contains(settings_cpp, "key == \"" + key + "\""),
              "bbport_settings.cpp parses '" + key + "'");
        // ...and stored: a member of that exact name.
        Check(Contains(settings_h, " " + field + "{") || Contains(settings_h, " " + field + " ={"),
              "bbport_settings.h declares '" + field + "'");
        // Written back, so a player's change survives the next start. `Save()` writes either the
        // value (`upscaler=%s`, `preset=%d`) or a name derived from it, so the key in a format
        // literal is the thing to look for.
        Check(Contains(settings_cpp, key + "=") || Contains(settings_cpp, field + ".load()"),
              "bbport_settings.cpp saves '" + key + "'");
        // Shown: the menu text, which is how a player reaches it at all.
        const std::string label = Str(row.label);
        Check(Contains(overlay, label), "the menu shows '" + label + "'");
    }

    // The community port's spelling of the motion-vector toggle stays a working alias.
    Check(Contains(settings_cpp, BbPortEffects::LessGhostingAlias),
          "bbport_settings.cpp accepts the 'less_ghosting' spelling");

    // --- bloom is not a placebo: it reaches the shader ----------------------
    // The table claims bloom works. These three checks are what "works" means: the pass carries the
    // value, the presenter fills it every frame, and the shader reads it. If any one is missing the
    // row is a checkbox that changes nothing, which is exactly what this test exists to catch.
    Check(Contains(pp_h, "float bloom"), "the post-process push constants carry 'bloom'");
    Check(Contains(presenter, "pp_settings.bloom"), "vk_presenter fills pp_settings.bloom");
    Check(Contains(shader, "pp.bloom"), "post_process.frag reads pp.bloom");
    Check(Contains(shader, "float bloom;"), "post_process.frag declares bloom in its push constants");
    // The layout contract: the block is four 4-byte members on the host side (asserted by
    // static_assert in pp_pass.h) and must be four 4-byte members here. A `bool` in the shader is 4
    // bytes today, but C++ `bool` is 1, so the two sides only stay in step while they agree on the
    // spelling - hence the check rather than a comment.
    const size_t block_at = shader.find("layout (push_constant) uniform settings {");
    if (block_at == std::string::npos) {
        Check(false, "post_process.frag has a push-constant block");
    } else {
        const std::string block = shader.substr(block_at, shader.find('}', block_at) - block_at);
        Check(!Contains(block, "bool "), "the push-constant block uses uint, not bool, for flags");
        Check(Contains(block, "hdr;") && Contains(block, "srgb_input;"),
              "the push-constant block still carries both flags");
    }

    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all checks passed (%d effects, %d runtime toggles)\n", BbPortEffects::EffectCount,
                BbPortEffects::RuntimeToggleCount);
    return 0;
}
