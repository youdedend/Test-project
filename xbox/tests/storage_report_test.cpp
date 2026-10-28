// SPDX-License-Identifier: GPL-2.0-or-later
// CI test for xbox/platform/storage_report.h: the storage report's portable half.
//
// The console half of this feature (which volumes exist, which of them the app may write to) needs
// the console. What can be held here is the arithmetic and the rules that decide whether a player
// is warned: the size formatting, the low-storage verdict, "pick the biggest writable volume other
// than the one we are on", and the folder walk that answers "what is LocalState spending the
// volume on". The walk is the part with a trap in it - a truncated walk must say so instead of
// reporting a number that is silently too small.

#include "storage_report.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

void CheckEqual(const std::string& actual, const std::string& expected, const std::string& what) {
    Check(actual == expected, what + " (got '" + actual + "', expected '" + expected + "')");
}

BbStorage::Volume Make(char letter, std::uint64_t free_bytes, bool writable) {
    BbStorage::Volume volume;
    volume.letter = letter;
    volume.total = free_bytes * 4;
    volume.free = free_bytes;
    volume.writable = writable;
    return volume;
}

} // namespace

int main() {
    // --- formatting -------------------------------------------------------
    CheckEqual(BbStorage::FormatBytes(0), "0 KiB", "zero");
    CheckEqual(BbStorage::FormatBytes(1536), "1 KiB", "kilobytes");
    CheckEqual(BbStorage::FormatBytes(std::uint64_t{512} << 20), "512 MiB", "megabytes");
    CheckEqual(BbStorage::FormatBytes(std::uint64_t{4} << 30), "4.0 GiB", "gigabytes");

    // --- the verdict ------------------------------------------------------
    Check(!BbStorage::LowStorage(std::uint64_t{4} << 30), "4 GiB free is not low");
    Check(BbStorage::LowStorage(std::uint64_t{867} << 20),
          "867 MiB free is low (the .819 volume that capped the swap file to 483 MiB)");
    Check(!BbStorage::LowStorage(BbStorage::kLowStorageBytes), "exactly the threshold is not low");
    Check(BbStorage::LowStorage(BbStorage::kLowStorageBytes - 1), "one byte under is low");

    // --- the volume with room, and the ones that only look like it --------
    {
        std::vector<BbStorage::Volume> volumes = {
            Make('Q', std::uint64_t{867} << 20, true),    // LocalState: small, writable
            Make('D', std::uint64_t{30} << 30, false),    // the big one: read-only to the app
            Make('E', std::uint64_t{2} << 30, true),      // the real alternative
            Make('F', 0, true),                          // writable, no space
        };
        const auto* best = BbStorage::BestWritable(volumes, 'Q');
        Check(best != nullptr && best->letter == 'E',
              "the biggest *writable* volume wins, not the biggest volume");
        Check(BbStorage::BestWritable(volumes, 'E') != nullptr &&
                  BbStorage::BestWritable(volumes, 'E')->letter == 'Q',
              "the volume being skipped is never the answer");
        std::vector<BbStorage::Volume> readonly = {Make('Q', std::uint64_t{1} << 30, true),
                                                   Make('D', std::uint64_t{30} << 30, false)};
        Check(BbStorage::BestWritable(readonly, 'Q') == nullptr,
              "a read-only volume is never recommended, however large");
    }

    // --- the folder walk --------------------------------------------------
    {
        std::error_code ec;
        const auto root = std::filesystem::temp_directory_path(ec) / "bbport_storage_walk_test";
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root / "Prepared" / "nested", ec);
        std::filesystem::create_directories(root / "Logs", ec);
        auto write = [](const std::filesystem::path& path, size_t bytes) {
            std::ofstream out(path, std::ios::binary);
            std::string block(bytes, 'x');
            out.write(block.data(), (std::streamsize)block.size());
        };
        write(root / "Prepared" / "patches.bin", 4000);
        write(root / "Prepared" / "nested" / "eboot.bin", 6000);
        write(root / "Logs" / "loader.log.txt", 1000);
        write(root / "bbport.ini", 500);

        BbStorage::FolderSize total;
        const auto folders = BbStorage::WalkFolders(root, total);
        Check(folders.size() == 3, "three entries: two folders and one loose file");
        std::uint64_t prepared = 0, logs = 0, ini = 0;
        for (const auto& folder : folders) {
            if (folder.name == "Prepared") prepared = folder.bytes;
            if (folder.name == "Logs") logs = folder.bytes;
            if (folder.name == "bbport.ini") ini = folder.bytes;
        }
        Check(prepared == 10000, "a folder's size includes its subfolders");
        Check(logs == 1000, "a folder's size is its files");
        Check(ini == 500, "a loose file is reported on its own line");
        Check(total.bytes == 11500, "the total is the sum of the entries");
        Check(!total.truncated, "a small tree is not truncated");

        // A walk with a cap must say it was capped rather than return a small number as if it were
        // the answer: that is the difference between a diagnostic and a lie.
        BbStorage::FolderSize capped_total;
        const auto capped = BbStorage::WalkFolders(root, capped_total, 2);
        bool any_truncated = false;
        for (const auto& folder : capped) any_truncated = any_truncated || folder.truncated;
        Check(any_truncated, "a capped walk is marked truncated");
        Check(capped_total.bytes <= total.bytes, "a capped walk never reports more than the full one");

        std::filesystem::remove_all(root, ec);
    }

    // --- a missing root is not a crash ------------------------------------
    {
        BbStorage::FolderSize total;
        const auto folders = BbStorage::WalkFolders(
            std::filesystem::path{"Z:/bbport/does/not/exist"}, total);
        Check(folders.empty() && total.bytes == 0, "a missing LocalState reports nothing");
    }

    // --- the warning text says what to do ---------------------------------
    Check(!BbStorage::WarningText().empty(), "the warning text exists for the dialog");

    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("storage report: all checks passed\n");
    return 0;
}
