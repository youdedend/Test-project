// SPDX-License-Identifier: GPL-2.0-or-later
// CI test for xbox/platform/overlay_index.h: the path rules behind the update overlay.
//
// The overlay redirects <dump>\<rel> to <update>\<rel> for every path the update provides, so a
// wrong rule shows up as either the base game's 01.00 data being used (no redirect) or the wrong
// file being read entirely. The cases below are the ones a real dump hits: the base folder is a
// prefix of the update folder name, guest paths mix '/' and '\', Windows paths are case-insensitive,
// the update carries paths deeper than the boot container and the base carries files the update does
// not.

#include "overlay_index.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

std::string Map(const BbOverlayIndex& index, const std::string& path) {
    char out[BB_OVERLAY_MAX_ROOT + BB_OVERLAY_MAX_REL];
    out[0] = '\0';
    return bb_overlay_maps_path(&index, path.c_str(), out, sizeof(out)) ? std::string(out)
                                                                        : std::string();
}

std::string MapDir(const BbOverlayIndex& index, const std::string& path) {
    char out[BB_OVERLAY_MAX_ROOT + BB_OVERLAY_MAX_REL];
    out[0] = '\0';
    return bb_overlay_maps_dir(&index, path.c_str(), out, sizeof(out)) ? std::string(out)
                                                                      : std::string();
}

const char* kBase = "E:\\dumps\\Bloodborne";
const char* kPatch = "E:\\dumps\\Bloodborne-patch";
const char* kList =
    "eboot.bin\n"
    "sce_sys/param.sfo\n"
    "sce_module/libc.prx\n"
    "dvdroot_ps4/map/m24_00.mapbnd\n"
    "dvdroot_ps4/event/common.emevd\n";

BbOverlayIndex Make() {
    BbOverlayIndex index{};
    const std::string list = kList;
    if (bb_overlay_parse(&index, kBase, kPatch, list.data(), list.size()) != 0) {
        std::printf("FATAL: the test list did not parse\n");
        std::exit(EXIT_FAILURE);
    }
    return index;
}

void TestMapping(const BbOverlayIndex& index) {
    CheckEqual(Map(index, "E:\\dumps\\Bloodborne\\eboot.bin"),
               "E:\\dumps\\Bloodborne-patch\\eboot.bin", "a boot file is redirected to the update");
    CheckEqual(Map(index, "E:\\dumps\\Bloodborne\\sce_sys/param.sfo"),
               "E:\\dumps\\Bloodborne-patch\\sce_sys\\param.sfo",
               "a mixed-separator path is redirected");
    CheckEqual(Map(index, "E:/dumps/Bloodborne/dvdroot_ps4/map/m24_00.mapbnd"),
               "E:\\dumps\\Bloodborne-patch\\dvdroot_ps4\\map\\m24_00.mapbnd",
               "a forward-slash path is redirected");
    CheckEqual(Map(index, "e:\\DUMPS\\bloodborne\\EBOOT.BIN"),
               "E:\\dumps\\Bloodborne-patch\\eboot.bin",
               "the match is case-insensitive (Windows paths are)");
}

void TestBaseOnlyFilesStayOnTheBase(const BbOverlayIndex& index) {
    CheckEqual(Map(index, "E:\\dumps\\Bloodborne\\dvdroot_ps4\\map\\m21_00.mapbnd"), "",
               "a base-only data file is not redirected");
    CheckEqual(Map(index, "E:\\dumps\\Bloodborne\\sce_module\\libSceFios2.prx"), "",
               "a base-only module is not redirected");
    CheckEqual(Map(index, "C:\\Data\\Users\\user\\savedata\\memory.dat"), "",
               "paths outside the dump are not redirected");
    CheckEqual(Map(index, "E:\\dumps\\Bloodborne"), "", "the dump root itself is not redirected");
}

void TestLookalikeFolders(const BbOverlayIndex& index) {
    // "Bloodborne" is a prefix of "Bloodborne-patch": the next character must be a separator.
    CheckEqual(Map(index, "E:\\dumps\\Bloodborne-patch\\eboot.bin"), "",
               "the update folder itself is not mistaken for the dump");
    CheckEqual(Map(index, "E:\\dumps\\Bloodborne2\\eboot.bin"), "",
               "a folder sharing the dump's prefix is not redirected");
}

void TestDirectories(const BbOverlayIndex& index) {
    CheckEqual(MapDir(index, "E:\\dumps\\Bloodborne\\dvdroot_ps4\\map"),
               "E:\\dumps\\Bloodborne-patch\\dvdroot_ps4\\map",
               "a directory is mapped to the update's directory");
    CheckEqual(MapDir(index, "E:\\dumps\\Bloodborne"), "", "the dump root is not mapped as a dir");
}

void TestNormalizedRoots() {
    BbOverlayIndex index{};
    const std::string list = kList;
    Check(bb_overlay_parse(&index, "E:\\dumps\\Bloodborne\\", "E:/dumps/Bloodborne-patch/",
                           list.data(), list.size()) == 0,
          "roots with mixed separators and a trailing separator parse");
    CheckEqual(std::string(index.base), "E:\\dumps\\Bloodborne", "the dump root is normalized");
    CheckEqual(std::string(index.patch), "E:\\dumps\\Bloodborne-patch",
               "the update root is normalized");
    CheckEqual(Map(index, "E:\\dumps\\Bloodborne\\eboot.bin"),
               "E:\\dumps\\Bloodborne-patch\\eboot.bin",
               "a trailing separator on the root does not break the redirect");
    bb_overlay_index_free(&index);
}

void TestCrlfAndBlankLines() {
    BbOverlayIndex index{};
    const char* list = "eboot.bin\r\n\r\nsce_sys/param.sfo\r\n";
    Check(bb_overlay_parse(&index, kBase, kPatch, list, std::strlen(list)) == 0,
          "CRLF line endings and blank lines parse");
    Check(index.count == 2, "blank lines are skipped");
    CheckEqual(Map(index, "E:\\dumps\\Bloodborne\\sce_sys\\param.sfo"),
               "E:\\dumps\\Bloodborne-patch\\sce_sys\\param.sfo",
               "a CRLF entry is redirected");
    bb_overlay_index_free(&index);
}

void TestEmptyAndBadLists() {
    BbOverlayIndex index{};
    Check(bb_overlay_parse(&index, kBase, kPatch, "", 0) != 0, "an empty list is rejected");
    Check(index.count == 0, "a rejected list leaves the index empty");
    CheckEqual(Map(index, "E:\\dumps\\Bloodborne\\eboot.bin"), "",
               "an empty index redirects nothing");
    Check(bb_overlay_parse(&index, "", kPatch, "eboot.bin\n", 11) != 0,
          "a missing dump root is rejected");
    Check(bb_overlay_parse(&index, kBase, "", "eboot.bin\n", 11) != 0,
          "a missing update root is rejected");
    bb_overlay_index_free(&index);
}

void TestOverlongPath(const BbOverlayIndex& index) {
    std::string deep = "E:\\dumps\\Bloodborne\\";
    deep.append(700, 'a');
    CheckEqual(Map(index, deep), "", "a relative path too long for the index is not redirected");
}

void TestReconfigure() {
    BbOverlayIndex index = Make();
    Check(index.count == 5, "the first list is loaded");
    const char* second = "eboot.bin\n";
    Check(bb_overlay_parse(&index, kBase, kPatch, second, std::strlen(second)) == 0,
          "a second list replaces the first");
    Check(index.count == 1, "the old entries are gone");
    CheckEqual(Map(index, "E:\\dumps\\Bloodborne\\sce_sys\\param.sfo"), "",
               "a path from the old list is no longer redirected");
    bb_overlay_index_free(&index);
}

void TestManyEntries() {
    std::string list;
    for (int i = 0; i < 5000; ++i) {
        list += "dvdroot_ps4/data/f" + std::to_string(i) + ".dat\n";
    }
    BbOverlayIndex index{};
    Check(bb_overlay_parse(&index, kBase, kPatch, list.data(), list.size()) == 0,
          "a real-sized index parses");
    Check(index.count == 5000, "every entry is kept");
    CheckEqual(Map(index, "E:\\dumps\\Bloodborne\\dvdroot_ps4\\data\\f4321.dat"),
               "E:\\dumps\\Bloodborne-patch\\dvdroot_ps4\\data\\f4321.dat",
               "an entry in the middle of the index is found");
    CheckEqual(Map(index, "E:\\dumps\\Bloodborne\\dvdroot_ps4\\data\\f5001.dat"), "",
               "an entry that is not in the index is not found");
    bb_overlay_index_free(&index);
}

} // namespace

int main() {
    const BbOverlayIndex index = Make();
    TestMapping(index);
    TestBaseOnlyFilesStayOnTheBase(index);
    TestLookalikeFolders(index);
    TestDirectories(index);
    TestOverlongPath(index);
    TestNormalizedRoots();
    TestCrlfAndBlankLines();
    TestEmptyAndBadLists();
    TestReconfigure();
    TestManyEntries();

    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("update overlay paths: all checks passed\n");
    return EXIT_SUCCESS;
}
