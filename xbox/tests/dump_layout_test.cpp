// SPDX-License-Identifier: GPL-2.0-or-later
// CI test for dump layout detection and the update overlay (xbox/platform/prepare_game.cpp:
// DetectDumpLayout, BuildOverlayedImage).
//
// It builds synthetic base game / update pairs with real PSF (param.sfo) files in a temporary
// directory and checks every layout the app has to support: merged, split (sibling and nested
// patch folders, preferred patch from bbport.ini), patch-rooted and base-only, plus the overlay
// that is materialized from a split pair (boot files from the update, base game for what the update
// lacks, game data left to the file-layer redirect, no writes to either source folder). The file
// layer the real app uses (bb_uwp_*) is provided here with std::filesystem, so the production code
// under test is exactly the one that ships.

#include "prepare_game.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ---- the four platform hooks prepare_game.cpp calls for file access -------------------------

namespace {
/// The production code builds Win32 paths ("dir\\sce_sys\\param.sfo"); the CI test also runs on
/// Linux where std::filesystem wants '/'. Windows accepts forward slashes too, so normalize here
/// instead of weakening the shipped code.
std::string Native(const char* path) {
    std::string text{path ? path : ""};
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}
/// Test-side comparison of a path the production code produced ("\\") with a std::filesystem
/// path ("/").
std::string Norm(const std::string& path) {
    std::string text = path;
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}
std::string LeafName(const std::string& path) {
    const std::string text = Norm(path);
    const size_t slash = text.find_last_of('/');
    return slash == std::string::npos ? text : text.substr(slash + 1);
}
} // namespace

extern "C" FILE* bb_uwp_fopen(const char* path, const char* mode) {
    return std::fopen(Native(path).c_str(), mode);
}

extern "C" int bb_uwp_is_file(const char* path) {
    std::error_code ec;
    return fs::is_regular_file(Native(path), ec) ? 1 : 0;
}

extern "C" int bb_uwp_is_dir(const char* path) {
    std::error_code ec;
    return fs::is_directory(Native(path), ec) ? 1 : 0;
}

extern "C" int bb_uwp_mkdir(const char* path) {
    std::error_code ec;
    fs::create_directories(Native(path), ec);
    return ec ? -1 : 0;
}

extern "C" int bb_uwp_delete(const char* path) {
    std::error_code ec;
    return fs::remove(Native(path), ec) ? 1 : 0;
}

extern "C" int bb_uwp_remove_directory(const char* path) {
    std::error_code ec;
    return fs::remove(Native(path), ec) ? 1 : 0;
}

extern "C" void bb_uwp_list_dir(const char* dir,
                                void (*visit)(const char* name, int is_dir, void* user),
                                void* user) {
    std::error_code ec;
    if (!dir || !visit) {
        return;
    }
    for (const auto& entry : fs::directory_iterator(Native(dir), ec)) {
        const std::string name = entry.path().filename().string();
        if (name == "." || name == "..") {
            continue;
        }
        visit(name.c_str(), entry.is_directory(ec) ? 1 : 0, user);
    }
}

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

[[noreturn]] void Die(const std::string& why) {
    std::printf("FATAL: %s\n", why.c_str());
    std::exit(EXIT_FAILURE);
}

// ---- minimal PSF writer ---------------------------------------------------------------------

void PutLe32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xff));
    }
}

void PutLe16(std::vector<uint8_t>& out, uint16_t value) {
    for (int i = 0; i < 2; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xff));
    }
}

void PutEntry(std::vector<uint8_t>& out, uint16_t key_offset, uint32_t size, uint32_t offset) {
    PutLe16(out, key_offset);
    PutLe16(out, 0x0204);  // UTF-8 string
    PutLe32(out, size);
    PutLe32(out, size + 1);
    PutLe32(out, offset);
}

std::vector<uint8_t> MakeSfo(const std::vector<std::pair<std::string, std::string>>& values) {
    std::vector<uint8_t> keys;
    std::vector<uint8_t> data;
    std::vector<uint8_t> index;
    for (const auto& [key, value] : values) {
        const uint16_t key_offset = static_cast<uint16_t>(keys.size());
        keys.insert(keys.end(), key.begin(), key.end());
        keys.push_back(0);
        const uint32_t data_offset = static_cast<uint32_t>(data.size());
        data.insert(data.end(), value.begin(), value.end());
        data.push_back(0);
        PutEntry(index, key_offset, static_cast<uint32_t>(value.size() + 1), data_offset);
    }
    std::vector<uint8_t> out;
    out.insert(out.end(), {'\0', 'P', 'S', 'F'});
    PutLe32(out, 0x0101);
    PutLe32(out, static_cast<uint32_t>(20 + index.size()));
    PutLe32(out, static_cast<uint32_t>(20 + index.size() + keys.size()));
    PutLe32(out, static_cast<uint32_t>(values.size()));
    out.insert(out.end(), index.begin(), index.end());
    out.insert(out.end(), keys.begin(), keys.end());
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

void WriteFile(const fs::path& path, const std::vector<uint8_t>& data) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        Die("cannot write " + path.string());
    }
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

/// Creates a bootable container (eboot.bin, sce_module PRXs, sce_sys/param.sfo, dvdroot_ps4).
fs::path MakeContainer(const fs::path& dir, const std::string& app_ver,
                       const std::string& app_type, const std::string& content_id,
                       const std::string& title_id = "CUSA03173") {
    std::error_code ec;
    fs::create_directories(dir, ec);
    WriteFile(dir / "eboot.bin", {1, 2, 3, 4});
    WriteFile(dir / "sce_module" / "libc.prx", {5, 6, 7});
    WriteFile(dir / "sce_module" / "libSceFios2.prx", {8, 9});
    WriteFile(dir / "dvdroot_ps4" / "map" / "m24.mapbnd", {10, 11});
    WriteFile(dir / "sce_sys" / "param.sfo",
              MakeSfo({{"APP_VER", app_ver},
                       {"APP_TYPE", app_type},
                       {"TITLE_ID", title_id},
                       {"TITLE", "Bloodborne"},
                       {"CONTENT_ID", content_id}}));
    return dir;
}

// ---- tests ----------------------------------------------------------------------------------

void TestReadParamSfo(const fs::path& root) {
    const auto dir = MakeContainer(root / "read" / "Bloodborne", "01.00", "gp",
                                   "UP9000-CUSA03173_00-BLOODBORNE0000000");
    const auto info = BbPrepare::ReadParamSfo(dir.string());
    Check(info.has_value(), "ReadParamSfo reads a synthetic param.sfo");
    if (!info) {
        return;
    }
    CheckEqual(info->app_ver, "01.00", "APP_VER is read verbatim");
    CheckEqual(info->app_type, "gp", "APP_TYPE is read");
    CheckEqual(info->title_id, "CUSA03173", "TITLE_ID is read");
    Check(!info->IsPatch(), "a base game param.sfo is not a patch");

    const auto patch = MakeContainer(root / "read" / "Bloodborne-patch", "01.09", "patch",
                                     "UP9000-CUSA03173_00-BLOODBORNE0000000-A0109-V0100");
    const auto patch_info = BbPrepare::ReadParamSfo(patch.string());
    Check(patch_info.has_value(), "ReadParamSfo reads the patch param.sfo");
    if (patch_info) {
        Check(patch_info->IsPatch(), "APP_TYPE=patch marks the container as an update");
        CheckEqual(patch_info->update_ver, "01.09", "CONTENT_ID yields the update version");
        CheckEqual(patch_info->Version(), "01.09", "the patch version is 01.09");
    }

    // A malformed SFO must not fall back to a version.
    const fs::path broken = root / "read" / "broken";
    std::error_code ec;
    fs::create_directories(broken / "sce_sys", ec);
    WriteFile(broken / "sce_sys" / "param.sfo", {'n', 'o', 't', 'a', ' ', 's', 'f', 'o'});
    Check(!BbPrepare::ReadParamSfo(broken.string()).has_value(),
          "a malformed param.sfo yields nullopt (no silent 01.00 fallback)");

    // A valid PSF without APP_VER is rejected too.
    const fs::path no_ver = root / "read" / "no-ver";
    WriteFile(no_ver / "sce_sys" / "param.sfo",
              MakeSfo({{"APP_TYPE", "gp"}, {"TITLE_ID", "CUSA03173"}}));
    Check(!BbPrepare::ReadParamSfo(no_ver.string()).has_value(),
          "a param.sfo without APP_VER is rejected");
}

void TestMergedLayout(const fs::path& root) {
    const auto merged = MakeContainer(root / "merged" / "Bloodborne", "01.09", "gp",
                                      "UP9000-CUSA03173_00-BLOODBORNE0000000-A0109-V0100");
    const auto layout = BbPrepare::DetectDumpLayout(merged.string());
    Check(layout.kind == BbPrepare::DumpLayout::Kind::Merged, "a 01.09 dump is detected as merged");
    CheckEqual(layout.effective_ver, "01.09", "merged dump runs 01.09");
    Check(layout.patch_path.empty(), "merged layout needs no separate patch folder");
    Check(layout.IsValid(), "merged layout is valid");
}

void TestMergedAboveTarget(const fs::path& root) {
    // Nothing about 01.09 is hard-coded: a dump that is already newer than the patch set's target
    // is still a merged dump and keeps its own version.
    const auto newer = MakeContainer(root / "newer" / "Bloodborne", "01.10", "gp",
                                     "UP9000-CUSA03173_00-BLOODBORNE0000000-A0110-V0100");
    const auto layout = BbPrepare::DetectDumpLayout(newer.string());
    Check(layout.kind == BbPrepare::DumpLayout::Kind::Merged,
          "a 01.10 dump is detected as merged");
    CheckEqual(layout.effective_ver, "01.10", "merged dump keeps its own version");
}

void TestPatchNotNewer(const fs::path& root) {
    // A leftover older/equal update folder must never downgrade a dump.
    const auto base = MakeContainer(root / "older-patch" / "Bloodborne", "01.05", "gp",
                                    "UP9000-CUSA03173_00-BLOODBORNE0000000-A0105-V0100");
    MakeContainer(root / "older-patch" / "Bloodborne-patch", "01.03", "patch",
                  "UP9000-CUSA03173_00-BLOODBORNE0000000-A0103-V0100");
    const auto layout = BbPrepare::DetectDumpLayout(base.string());
    Check(layout.kind == BbPrepare::DumpLayout::Kind::BaseOnly,
          "an update folder that is not newer is ignored");
    CheckEqual(layout.effective_ver, "01.05", "the dump keeps its own newer version");
    Check(layout.patch_path.empty(), "no patch path is reported for an ignored update folder");
}

void TestMergedNextToOldBase(const fs::path& root) {
    // A patched-in-place dump that still has the original 01.00 folder next to it.
    const auto base = MakeContainer(root / "side-by-side" / "Bloodborne-0100", "01.00", "gp",
                                    "UP9000-CUSA03173_00-BLOODBORNE0000000");
    const auto merged = MakeContainer(root / "side-by-side" / "Bloodborne", "01.09", "gp",
                                      "UP9000-CUSA03173_00-BLOODBORNE0000000-A0109-V0100");
    const auto merged_layout = BbPrepare::DetectDumpLayout(merged.string());
    Check(merged_layout.kind == BbPrepare::DumpLayout::Kind::Merged,
          "a merged dump next to the old base folder stays merged");
    CheckEqual(merged_layout.effective_ver, "01.09", "the merged dump still runs 01.09");
    // Picking the old base folder next to the merged one pairs them correctly.
    const auto base_layout = BbPrepare::DetectDumpLayout(base.string());
    Check(base_layout.kind == BbPrepare::DumpLayout::Kind::Split,
          "the old base folder pairs with the patched folder next to it");
    CheckEqual(base_layout.effective_ver, "01.09", "the pair runs 01.09");
}

void TestSplitLayoutSibling(const fs::path& root) {
    const auto base = MakeContainer(root / "split" / "Bloodborne", "01.00", "gp",
                                    "UP9000-CUSA03173_00-BLOODBORNE0000000");
    MakeContainer(root / "split" / "Bloodborne-patch", "01.09", "patch",
                  "UP9000-CUSA03173_00-BLOODBORNE0000000-A0109-V0100");
    const auto layout = BbPrepare::DetectDumpLayout(base.string());
    Check(layout.kind == BbPrepare::DumpLayout::Kind::Split,
          "a 01.00 base with a sibling patch folder is detected as split");
    CheckEqual(layout.effective_ver, "01.09", "split layout runs the patch version");
    CheckEqual(LeafName(layout.patch_path), "Bloodborne-patch",
               "the detected patch folder is the sibling");
    CheckEqual(layout.base.app_ver, "01.00", "the base version is reported as 01.00");
    CheckEqual(layout.patch.Version(), "01.09", "the patch version is reported as 01.09");
}

void TestSplitLayoutNested(const fs::path& root) {
    const auto base = MakeContainer(root / "nested" / "Bloodborne", "01.00", "gp",
                                    "UP9000-CUSA03173_00-BLOODBORNE0000000");
    MakeContainer(base / "update", "01.09", "patch",
                  "UP9000-CUSA03173_00-BLOODBORNE0000000-A0109-V0100");
    const auto layout = BbPrepare::DetectDumpLayout(base.string());
    Check(layout.kind == BbPrepare::DumpLayout::Kind::Split,
          "a patch folder inside the dump is detected as split");
    CheckEqual(LeafName(layout.patch_path), "update",
               "the nested update folder is the patch source");
}

void TestHighestVersionWins(const fs::path& root) {
    const auto base = MakeContainer(root / "versions" / "Bloodborne", "01.00", "gp",
                                    "UP9000-CUSA03173_00-BLOODBORNE0000000");
    MakeContainer(root / "versions" / "Bloodborne-patch-0100", "01.03", "patch",
                  "UP9000-CUSA03173_00-BLOODBORNE0000000-A0103-V0100");
    MakeContainer(root / "versions" / "Bloodborne-patch-0109", "01.09", "patch",
                  "UP9000-CUSA03173_00-BLOODBORNE0000000-A0109-V0100");
    const auto layout = BbPrepare::DetectDumpLayout(base.string());
    CheckEqual(layout.effective_ver, "01.09", "the highest patch version wins");
    CheckEqual(LeafName(layout.patch_path), "Bloodborne-patch-0109",
               "the 01.09 folder is selected over the 01.03 one");
}

void TestPreferredPatch(const fs::path& root) {
    const auto base = MakeContainer(root / "preferred" / "Bloodborne", "01.00", "gp",
                                    "UP9000-CUSA03173_00-BLOODBORNE0000000");
    const auto elsewhere = MakeContainer(root / "elsewhere" / "MyPatch", "01.09", "patch",
                                         "UP9000-CUSA03173_00-BLOODBORNE0000000-A0109-V0100");
    const auto layout = BbPrepare::DetectDumpLayout(base.string(), elsewhere.string());
    Check(layout.kind == BbPrepare::DumpLayout::Kind::Split,
          "a patch path stored in bbport.ini is used even outside the dump folder");
    CheckEqual(Norm(layout.patch_path), elsewhere.string(), "the preferred patch path is kept");
    CheckEqual(layout.effective_ver, "01.09", "the preferred patch supplies the version");
}

void TestWrongTitleRejected(const fs::path& root) {
    const auto base = MakeContainer(root / "wrong" / "Bloodborne", "01.00", "gp",
                                    "UP9000-CUSA03173_00-BLOODBORNE0000000");
    MakeContainer(root / "wrong" / "another-patch", "01.09", "patch",
                  "UP9000-CUSA00900_00-BLOODBORNE0000000-A0109-V0100", "CUSA00900");
    const auto layout = BbPrepare::DetectDumpLayout(base.string());
    Check(layout.kind == BbPrepare::DumpLayout::Kind::BaseOnly,
          "a patch for a different TITLE_ID is not used");
    CheckEqual(layout.effective_ver, "01.00", "base-only layout keeps the base version");
}

void TestBaseOnly(const fs::path& root) {
    const auto base = MakeContainer(root / "baseonly" / "Bloodborne", "01.00", "gp",
                                    "UP9000-CUSA03173_00-BLOODBORNE0000000");
    const auto layout = BbPrepare::DetectDumpLayout(base.string());
    Check(layout.kind == BbPrepare::DumpLayout::Kind::BaseOnly,
          "a 01.00 dump without a patch is base-only");
    CheckEqual(layout.effective_ver, "01.00", "base-only effective version is 01.00");
    Check(layout.patch_path.empty(), "base-only has no patch path");
    Check(layout.Summary().find("SKIPPED") != std::string::npos,
          "the base-only summary warns that patches are skipped");
}

void TestPatchRooted(const fs::path& root) {
    const auto base = MakeContainer(root / "rooted" / "Bloodborne", "01.00", "gp",
                                    "UP9000-CUSA03173_00-BLOODBORNE0000000");
    const auto patch = MakeContainer(root / "rooted" / "Bloodborne-patch", "01.09", "patch",
                                     "UP9000-CUSA03173_00-BLOODBORNE0000000-A0109-V0100");
    const auto layout = BbPrepare::DetectDumpLayout(patch.string());
    Check(layout.kind == BbPrepare::DumpLayout::Kind::Split,
          "picking the patch folder pairs it with the sibling base game");
    CheckEqual(layout.effective_ver, "01.09", "a patch-rooted pick runs 01.09");
    CheckEqual(Norm(layout.base_path), base.string(), "the sibling base folder supplies the data");
    CheckEqual(Norm(layout.patch_path), patch.string(),
               "the picked patch folder is the executable source");
}

std::string ReadText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void TestOverlayImage(const fs::path& root) {
    const auto dir = root / "overlay";
    const auto base = MakeContainer(dir / "Bloodborne", "01.00", "gp",
                                    "UP9000-CUSA03173_00-BLOODBORNE0000000");
    const auto patch = MakeContainer(dir / "Bloodborne-patch", "01.09", "patch",
                                     "UP9000-CUSA03173_00-BLOODBORNE0000000-A0109-V0100");
    WriteFile(base / "eboot.bin", {'b', 'a', 's', 'e'});
    WriteFile(patch / "eboot.bin", {'p', 'a', 't', 'c', 'h'});
    WriteFile(patch / "sce_module" / "libc.prx", {'p', 'l'});
    WriteFile(patch / "dvdroot_ps4" / "map" / "new_asset.bnd", {'n', 'e', 'w'});
    std::error_code ec;
    fs::remove(patch / "sce_module" / "libSceFios2.prx", ec);  // only the base game has it

    const auto out = dir / "Prepared" / "overlay";
    const auto image = BbPrepare::BuildOverlayedImage(base.string(), patch.string(), out.string());
    Check(image.IsValid(), "an update over a base dump yields a usable overlay");
    CheckEqual(ReadText(out / "eboot.bin"), "patch", "the update's eboot.bin wins");
    CheckEqual(ReadText(out / "sce_module" / "libc.prx"), "pl", "the update's module is copied");
    CheckEqual(ReadText(out / "sce_module" / "libSceFios2.prx"),
               ReadText(base / "sce_module" / "libSceFios2.prx"),
               "a module only the base game has is still present");
    Check(!fs::exists(out / "dvdroot_ps4" / "map" / "new_asset.bnd"),
          "game data is not copied into the overlay");
    const auto sfo = BbPrepare::ReadParamSfo(out.string());
    Check(sfo.has_value() && sfo->Version() == "01.09",
          "the overlay's param.sfo is the update's (01.09)");
    const std::string index = ReadText(out / "files.txt");
    Check(index.find("dvdroot_ps4/map/new_asset.bnd") != std::string::npos,
          "the update's data file is listed for the file-layer overlay");
    Check(index.find("eboot.bin") != std::string::npos, "the boot files are listed too");
    CheckEqual(ReadText(base / "eboot.bin"), "base", "the game dump is never modified");
    CheckEqual(ReadText(patch / "eboot.bin"), "patch", "the update folder is never modified");
}

void TestOverlayRebuildDropsStaleFiles(const fs::path& root) {
    const auto dir = root / "overlay-rebuild";
    const auto base = MakeContainer(dir / "Bloodborne", "01.00", "gp",
                                    "UP9000-CUSA03173_00-BLOODBORNE0000000");
    const auto patch = MakeContainer(dir / "Bloodborne-patch", "01.09", "patch",
                                     "UP9000-CUSA03173_00-BLOODBORNE0000000-A0109-V0100");
    const auto out = dir / "Prepared" / "overlay";
    WriteFile(patch / "sce_module" / "stale.prx", {'s'});
    auto first = BbPrepare::BuildOverlayedImage(base.string(), patch.string(), out.string());
    Check(first.IsValid() && fs::exists(out / "sce_module" / "stale.prx"),
          "the first overlay carries the update's extra module");

    std::error_code ec;
    fs::remove(patch / "sce_module" / "stale.prx", ec);
    auto second = BbPrepare::BuildOverlayedImage(base.string(), patch.string(), out.string());
    Check(second.IsValid(), "the overlay rebuilds");
    Check(!fs::exists(out / "sce_module" / "stale.prx"),
          "a module from the earlier overlay is not left behind");
    Check(ReadText(out / "files.txt").find("stale.prx") == std::string::npos,
          "the stale module is gone from the redirect list too");
}

void TestOverlayWithoutUpdate(const fs::path& root) {
    const auto dir = root / "overlay-none";
    const auto base = MakeContainer(dir / "Bloodborne", "01.00", "gp",
                                    "UP9000-CUSA03173_00-BLOODBORNE0000000");
    const auto image =
        BbPrepare::BuildOverlayedImage(base.string(), (dir / "missing").string(),
                                       (dir / "Prepared" / "overlay").string());
    Check(!image.IsValid(), "a missing update folder yields no overlay");
    Check(!fs::exists(dir / "Prepared" / "overlay" / "eboot.bin"),
          "no overlay files are written for a missing update folder");
}

void TestUnreadableDump(const fs::path& root) {
    const fs::path empty = root / "empty";
    std::error_code ec;
    fs::create_directories(empty, ec);
    const auto layout = BbPrepare::DetectDumpLayout(empty.string());
    Check(!layout.IsValid(), "a folder without param.sfo yields an invalid layout");
}

} // namespace

int main() {
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / "bb_dump_layout_test";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    TestReadParamSfo(root);
    TestMergedLayout(root);
    TestMergedAboveTarget(root);
    TestPatchNotNewer(root);
    TestMergedNextToOldBase(root);
    TestSplitLayoutSibling(root);
    TestSplitLayoutNested(root);
    TestHighestVersionWins(root);
    TestPreferredPatch(root);
    TestWrongTitleRejected(root);
    TestBaseOnly(root);
    TestPatchRooted(root);
    TestOverlayImage(root);
    TestOverlayRebuildDropsStaleFiles(root);
    TestOverlayWithoutUpdate(root);
    TestUnreadableDump(root);

    fs::remove_all(root, ec);
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("dump layout detection: all checks passed\n");
    return EXIT_SUCCESS;
}
