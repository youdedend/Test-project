// SPDX-License-Identifier: GPL-2.0-or-later
// CI test for xbox/platform/ini_file.h: the bbport.ini surgery behind "[dump] patch_path".
//
// The launcher rewrites two keys in the player's settings file every time a patch folder is picked,
// so the file must round-trip: comments, order and every unrelated setting stay byte-identical, a
// missing key lands inside the right section, a missing section is appended, and nothing is written
// when the value already matches. A regression here is what "my patch folder is forgotten after a
// restart" would look like on the console.

#include "ini_file.h"

#include <cstdio>
#include <cstdlib>
#include <string>

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

void CheckEqual(const std::string& actual, const std::string& expected, const std::string& what) {
    Check(actual == expected, what + " (got '" + actual + "', expected '" + expected + "')");
}

const char* kPlayerIni =
    "# Bloodborne Xbox Series X|S UWP settings (in-game overlay: L3 + R3)\n"
    "upscaler=off\n"
    "scene_res=1280x720\n"
    "fps_patch=60\n"
    "effect_ssr=0\n"
    "\n"
    "[dump]\n"
    "# Empty patch_path auto-detects the update folder next to the game folder.\n"
    "patch_path=\n"
    "use_patch=1\n";

void TestParseFlat() {
    const auto values = BbIni::Parse("upscaler=off\nfps_patch = 60 \n# comment\n; another\n");
    CheckEqual(values.at("upscaler"), "off", "a flat key is parsed");
    CheckEqual(values.at("fps_patch"), "60", "surrounding spaces are trimmed");
    Check(!values.count("# comment"), "comments are skipped");
    Check(!values.count("; another"), "semicolon comments are skipped");
}

void TestParseSections() {
    const auto values = BbIni::Parse(kPlayerIni);
    CheckEqual(values.at("fps_patch"), "60", "a global key still resolves by its bare name");
    CheckEqual(values.at("dump.use_patch"), "1", "[dump] qualifies its keys");
    CheckEqual(values.at("dump.patch_path"), "", "an empty value parses as empty");
    Check(BbIni::Flag(values, "dump.use_patch", false), "use_patch=1 reads as on");
    Check(!BbIni::Flag(values, "dump.patch_path", false), "an empty value takes the fallback");
}

void TestFlagValues() {
    const auto values = BbIni::Parse("a=1\nb=0\nc=off\nd=on\ne=false\nf=yes\ng=\n");
    Check(BbIni::Flag(values, "a", false), "1 is on");
    Check(!BbIni::Flag(values, "b", true), "0 is off");
    Check(!BbIni::Flag(values, "c", true), "off is off");
    Check(BbIni::Flag(values, "d", false), "on is on");
    Check(!BbIni::Flag(values, "e", true), "false is off");
    Check(BbIni::Flag(values, "f", false), "yes is on");
    Check(BbIni::Flag(values, "g", true), "an empty value uses the fallback");
    Check(BbIni::Flag(values, "missing", true), "a missing key uses the fallback");
}

void TestWriteExistingKey() {
    bool changed = false;
    const std::string out =
        BbIni::SetValue(kPlayerIni, "dump", "patch_path", "E:\\dumps\\Bloodborne-patch", &changed);
    Check(changed, "writing a new value reports a change");
    Check(out.find("patch_path=E:\\dumps\\Bloodborne-patch\n") != std::string::npos,
          "the value is written");
    Check(out.find("# Empty patch_path auto-detects") != std::string::npos,
          "the comment above the key survives");
    Check(out.find("upscaler=off\n") != std::string::npos, "unrelated settings survive");
    Check(out.find("[dump]\n") != std::string::npos, "the section header survives exactly once");
    CheckEqual(std::to_string(out.find("[dump]")), std::to_string(out.rfind("[dump]")),
               "the section is not duplicated");
    const auto reread = BbIni::Parse(out);
    CheckEqual(reread.at("dump.patch_path"), "E:\\dumps\\Bloodborne-patch",
               "the written path reads back (survives a restart)");
}

void TestWriteSameValue() {
    bool changed = true;
    const std::string out = BbIni::SetValue(kPlayerIni, "dump", "use_patch", "1", &changed);
    Check(!changed, "writing the value that is already there reports no change");
    CheckEqual(out, std::string(kPlayerIni), "the file text is unchanged");
}

void TestInsertMissingKey() {
    const std::string base = "upscaler=off\n\n[dump]\npatch_path=E:\\p\n";
    bool changed = false;
    const std::string out = BbIni::SetValue(base, "dump", "use_patch", "0", &changed);
    Check(changed, "a new key reports a change");
    const size_t patch = out.find("patch_path=E:\\p");
    const size_t use = out.find("use_patch=0");
    Check(patch != std::string::npos && use != std::string::npos && use > patch,
          "a new key is appended inside the existing section");
    CheckEqual(BbIni::Parse(out).at("dump.use_patch"), "0", "the inserted key reads back");
}

void TestAppendMissingSection() {
    const std::string base = "upscaler=off\n";
    bool changed = false;
    const std::string out = BbIni::SetValue(base, "dump", "patch_path", "E:\\p", &changed);
    Check(changed, "appending a section reports a change");
    Check(out.rfind("upscaler=off\n") == 0, "existing settings come first");
    Check(out.find("\n[dump]\npatch_path=E:\\p\n") != std::string::npos,
          "the section and key are appended");
    CheckEqual(BbIni::Parse(out).at("dump.patch_path"), "E:\\p", "the appended key reads back");
}

void TestEmptyFile() {
    bool changed = false;
    const std::string out = BbIni::SetValue("", "dump", "patch_path", "E:\\p", &changed);
    Check(changed, "writing into an empty file reports a change");
    CheckEqual(out, std::string("[dump]\npatch_path=E:\\p\n"), "the section is created");
}

void TestCrlfInput() {
    const std::string base = "upscaler=off\r\n\r\n[dump]\r\nuse_patch=1\r\n";
    bool changed = false;
    const std::string out = BbIni::SetValue(base, "dump", "patch_path", "E:\\p", &changed);
    Check(changed, "a CRLF file is rewritten");
    Check(out.find('\r') == std::string::npos, "the rewritten file uses LF only");
    const auto values = BbIni::Parse(out);
    CheckEqual(values.at("upscaler"), "off", "the CRLF setting survives");
    CheckEqual(values.at("dump.patch_path"), "E:\\p", "the new key lands in the section");
}

void TestClearingPatchPath() {
    const std::string base = "[dump]\npatch_path=E:\\p\nuse_patch=1\n";
    bool changed = false;
    const std::string out = BbIni::SetValue(base, "dump", "patch_path", "", &changed);
    Check(changed, "clearing the path reports a change");
    CheckEqual(BbIni::Parse(out).at("dump.patch_path"), "", "the path is cleared");
    CheckEqual(BbIni::Parse(out).at("dump.use_patch"), "1", "the toggle is left alone");
}


void TestTopLevelInsert() {
    const std::string base = "# player settings\nupscaler=off\n\n[dump]\nuse_patch=1\n";
    bool changed = false;
    const std::string out = BbIni::SetTopLevel(base, "skip_intro", "1", &changed);
    Check(changed, "a new top-level key reports a change");
    const auto values = BbIni::Parse(out);
    CheckEqual(values.at("skip_intro"), "1", "the key reads back");
    CheckEqual(values.at("dump.use_patch"), "1", "the section below is untouched");
    Check(out.find("[dump]\nuse_patch=1") != std::string::npos, "section order is kept");
    Check(out.find("upscaler=off\nskip_intro=1") != std::string::npos,
          "the key lands at the end of the top-level block");
}

void TestTopLevelReplace() {
    const std::string base = "skip_intro=0\n# comment\nfps_cap=60\n[dump]\nskip_intro=9\n";
    bool changed = false;
    const std::string out = BbIni::SetTopLevel(base, "skip_intro", "1", &changed);
    Check(changed, "changing a top-level key reports a change");
    Check(out.find("# comment") != std::string::npos, "comments survive");
    const auto values = BbIni::Parse(out);
    CheckEqual(values.at("fps_cap"), "60", "other keys survive");
    // A key of the same name inside a section keeps its own line; the bare key is the one written.
    Check(out.find("[dump]\nskip_intro=9") != std::string::npos,
          "the section's own key is not rewritten");
    CheckEqual(BbIni::Parse(out).at("dump.skip_intro"), "9",
               "and the section still answers for it");
}

void TestTopLevelDuplicate() {
    const std::string base = "a=1\nskip_intro=0\nb=2\nskip_intro=0\n[c]\nk=v\n";
    bool changed = false;
    const std::string out = BbIni::SetTopLevel(base, "skip_intro", "1", &changed);
    Check(changed, "a duplicate key reports a change");
    size_t count = 0;
    for (size_t at = out.find("skip_intro="); at != std::string::npos;
         at = out.find("skip_intro=", at + 1)) {
        ++count;
    }
    Check(count == 1, "duplicates collapse to one line");
    CheckEqual(BbIni::Parse(out).at("skip_intro"), "1", "the surviving line wins");
    CheckEqual(BbIni::Parse(out).at("c.k"), "v", "sections survive");
}

void TestTopLevelSameValue() {
    const std::string base = "skip_intro=1\n[dump]\npatch_path=\n";
    bool changed = true;
    BbIni::SetTopLevel(base, "skip_intro", "1", &changed);
    Check(!changed, "writing the same value reports no change");
}

void TestTopLevelEmptyFile() {
    bool changed = false;
    const std::string out = BbIni::SetTopLevel("", "skip_intro", "1", &changed);
    Check(changed, "an empty file gains the key");
    CheckEqual(out, std::string("skip_intro=1\n"), "and nothing else");
}

} // namespace

int main() {
    TestParseFlat();
    TestParseSections();
    TestFlagValues();
    TestWriteExistingKey();
    TestWriteSameValue();
    TestInsertMissingKey();
    TestAppendMissingSection();
    TestEmptyFile();
    TestCrlfInput();
    TestClearingPatchPath();
    TestTopLevelInsert();
    TestTopLevelReplace();
    TestTopLevelDuplicate();
    TestTopLevelSameValue();
    TestTopLevelEmptyFile();

    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("bbport.ini round-trip: all checks passed\n");
    return EXIT_SUCCESS;
}
