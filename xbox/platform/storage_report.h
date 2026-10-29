// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: what the console's storage actually looks like, and what LocalState is spending it on.
//
// `.819` ended the way it did because the swap file's volume had 867 MiB free while the console had
// 30 GB elsewhere: the pager was capped to 483 MiB, the volume filled, 6,828 writes were refused and
// the app died at 5,116/5,120 MiB with 1,171 MiB of pageable memory still resident. Nothing in the
// log said any of that - the cap line reported the number it chose, not the one next to it.
//
// So this reports every volume the app can see (total, free, and whether the app may write there -
// measured by creating a file, not guessed from the drive letter), and the size of each top-level
// folder in LocalState, which is where the swap file, the pipeline cache, the prepared dump and the
// game's own saves share one volume. It also answers "is there a better volume for the swap file"
// with a name and a number rather than a suggestion.
//
// The pure parts (formatting, the verdict, the folder walk) are inline and free of Win32 so they are
// testable off-console; storage_report.cpp is the part that calls the OS.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace BbStorage {

enum class VolumeKind { Unknown, Fixed, Removable, Other };
// A fixed drive can still be an external USB hard disk; media type alone does not prove the bus.
enum class VolumeBus { Unknown, NonUsb, UsbOrRemovable };

struct Volume {
    char letter = 0;          ///< 'Q', 'D', ...
    std::uint64_t total = 0;  ///< bytes, 0 when the query failed
    std::uint64_t free = 0;
    bool writable = false;    ///< a 64 KiB probe file was created and removed here
    VolumeKind kind = VolumeKind::Unknown; ///< unknown is NOT considered safe for automatic swap
    VolumeBus bus = VolumeBus::Unknown;   ///< unknown/USB must not be chosen automatically
};

struct SwapChoice {
    std::string path;
    std::string source;  ///< environment, ini, automatic, or LocalState fallback
    char letter = 0;
    std::uint64_t free = 0;
    bool local = true;
};

struct FolderSize {
    std::string name;
    std::uint64_t bytes = 0;
    std::uint64_t files = 0;
    bool truncated = false;   ///< the walk hit its entry cap (size is then a lower bound)
};

/// Below this much free space on the volume that holds LocalState, the run is warned about before it
/// starts rather than at the point where the pager is capped to it.
inline constexpr std::uint64_t kLowStorageBytes = std::uint64_t{1024} << 20;  // 1 GiB

/// Human-readable size, for the log and the warning. One decimal below a GiB, none above.
inline std::string FormatBytes(std::uint64_t bytes) {
    char text[32];
    if (bytes >= (std::uint64_t{1} << 30)) {
        std::snprintf(text, sizeof(text), "%.1f GiB", double(bytes) / double(std::uint64_t{1} << 30));
    } else if (bytes >= (std::uint64_t{1} << 20)) {
        std::snprintf(text, sizeof(text), "%llu MiB",
                      static_cast<unsigned long long>(bytes >> 20));
    } else {
        std::snprintf(text, sizeof(text), "%llu KiB",
                      static_cast<unsigned long long>(bytes >> 10));
    }
    return text;
}

/// The verdict for one volume: is this run about to be limited by storage?
inline bool LowStorage(std::uint64_t free_bytes) {
    return free_bytes < kLowStorageBytes;
}

/// The volume with the most free space that the app may write to, or nullptr. `skip` is the letter
/// of the volume to ignore (the app volume itself, when the caller wants an alternative, or the USB
/// drive holding the dump - writing the swap file there would be both slow and rude).
inline const Volume* BestWritable(const std::vector<Volume>& volumes, char skip) {
    const Volume* best = nullptr;
    for (const Volume& volume : volumes) {
        if (!volume.writable || volume.letter == skip) continue;
        if (!best || volume.free > best->free) best = &volume;
    }
    return best;
}

/// Automatic swap placement: only an independently probed writable FIXED volume may beat
/// LocalState. DRIVE_FIXED alone does not rule out a USB HDD: require a confirmed non-USB bus too;
/// unknown/removable/remote devices are rejected.
/// D wins a tie, otherwise the one with most free bytes wins. Below the reserve it is unusable.
inline const Volume* BestSwapVolume(const std::vector<Volume>& volumes, char local,
                                     std::uint64_t reserve) {
    const Volume* best = nullptr;
    for (const Volume& volume : volumes) {
        if (!volume.writable || volume.letter == local || volume.kind != VolumeKind::Fixed ||
            volume.bus != VolumeBus::NonUsb || volume.free <= reserve) continue;
        if (!best || volume.free > best->free ||
            (volume.free == best->free && volume.letter == 'D' && best->letter != 'D')) {
            best = &volume;
        }
    }
    return best;
}

/// Select a location (including strict removable-drive and existing-file checks). The caller applies
/// env/ini precedence and provides the maximum free-space reserve. No file is overwritten.
SwapChoice ChooseSwap(const std::string& local_dir, const std::string& requested_path,
                      const char* requested_source, bool auto_select, std::uint64_t reserve);

/// An opt-in 64 MiB temporary-file throughput test on the selected volume. Logs actual write/read
/// timings or why it could not safely run. The file uses CREATE_NEW + DELETE_ON_CLOSE.
void TestSwapSpeed(const SwapChoice& choice, std::uint64_t reserve, bool enabled);

/// Size of every immediate child of `root`, files counted through the whole subtree.
/// `cap` bounds the number of entries examined, and a folder that hits it is marked `truncated` so a
/// huge tree cannot turn a startup diagnostic into a startup delay. Missing folders are skipped.
inline std::vector<FolderSize> WalkFolders(const std::filesystem::path& root, FolderSize& root_total,
                                           std::uint64_t cap = 200000) {
    std::vector<FolderSize> out;
    root_total = FolderSize{};
    root_total.name = "LocalState";
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        return out;
    }
    std::vector<std::filesystem::path> children;
    for (std::filesystem::directory_iterator it{root, ec}, end; !ec && it != end; it.increment(ec)) {
        children.push_back(it->path());
    }
    std::uint64_t budget = cap;
    for (const auto& child : children) {
        FolderSize entry;
        entry.name = child.filename().string();
        if (std::filesystem::is_directory(child, ec)) {
            for (std::filesystem::recursive_directory_iterator it{child, ec}, end;
                 !ec && it != end; it.increment(ec)) {
                if (budget == 0) {
                    entry.truncated = true;
                    break;
                }
                --budget;
                std::error_code size_ec;
                if (it->is_regular_file(size_ec)) {
                    entry.bytes += std::filesystem::file_size(it->path(), size_ec);
                    ++entry.files;
                }
            }
            ec.clear();
        } else {
            std::error_code size_ec;
            entry.bytes = std::filesystem::file_size(child, size_ec);
            entry.files = entry.bytes ? 1 : 0;
        }
        root_total.bytes += entry.bytes;
        root_total.files += entry.files;
        root_total.truncated = root_total.truncated || entry.truncated;
        out.push_back(std::move(entry));
    }
    return out;
}

/// One line per volume, one per folder, and the recommendation. `local_dir` is the UTF-8 LocalState
/// path. Returns 1 when storage is low (the caller shows the warning), 0 otherwise.
int Report(const char* local_dir);

/// Free bytes on the volume that holds LocalState. Cached for a second: the heartbeat asks every
/// sample and the answer cannot change faster than a write can fill it.
std::uint64_t LocalStateFreeBytes();

/// The text of the visible warning. Inline, so the test can hold its wording without linking the
/// OS half: it is what the player is asked to act on, and "low storage" alone would not say what to
/// free or why the game cares.
inline std::string WarningText() {
    return std::string("Low storage: the volume holding the game's data has less than ") +
           FormatBytes(kLowStorageBytes) +
           " free. Saves and caches also need that space even if swap is on another volume. "
           "Free space on that volume (old logs, captures, other games) before playing.";
}

} // namespace BbStorage
