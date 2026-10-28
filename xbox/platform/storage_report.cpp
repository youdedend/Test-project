// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the OS half of the storage report (storage_report.h holds the testable half).
//
// Every volume is probed the same way: GetDiskFreeSpaceExW for the numbers, then a 64 KiB file that
// is written, flushed and deleted. The probe is what makes the answer real - a UWP app is allowed to
// read most volumes and write almost none of them, and "24 GB free on D:" is worse than useless if
// the swap file cannot live there. The probe file is opened with FILE_FLAG_DELETE_ON_CLOSE so a
// crash cannot leave litter on a player's drive.

#include "storage_report.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace BbStorage {
namespace {

constexpr std::uint64_t kProbeBytes = 64 * 1024;

/// The LocalState path Report() was called with, for LocalStateFreeBytes().
std::string g_local_dir;

/// The app's own volume letter from a root path like `C:\Users\...\LocalState`.
char LetterOf(const std::string& path) {
    if (path.size() >= 2 && path[1] == ':') {
        char letter = path[0];
        if (letter >= 'a' && letter <= 'z') letter = char(letter - 'a' + 'A');
        return letter;
    }
    return 0;
}

std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide((size_t)length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), wide.data(), length);
    return wide;
}

bool ProbeWritable(const std::wstring& root) {
    const std::wstring path = root + L"bbport_storage_probe.tmp";
    CREATEFILE2_EXTENDED_PARAMETERS params{};
    params.dwSize = sizeof(params);
    params.dwFileAttributes = FILE_ATTRIBUTE_TEMPORARY;
    params.dwFileFlags = FILE_FLAG_DELETE_ON_CLOSE;
    const HANDLE file = CreateFile2FromAppW(path.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, &params);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    std::vector<char> block(kProbeBytes, 0x5a);
    DWORD written = 0;
    const bool ok = WriteFile(file, block.data(), (DWORD)block.size(), &written, nullptr) != FALSE &&
                    written == block.size();
    CloseHandle(file);
    return ok;
}

std::vector<Volume> QueryVolumes(char local_letter) {
    std::vector<Volume> volumes;
    for (char letter = 'C'; letter <= 'Z'; ++letter) {
        std::wstring root;
        root.push_back((wchar_t)letter);
        root += L":\\";
        ULARGE_INTEGER free_bytes{}, total_bytes{};
        if (!GetDiskFreeSpaceExW(root.c_str(), &free_bytes, &total_bytes, nullptr)) {
            continue;  // no such volume, or not visible to this app
        }
        if (total_bytes.QuadPart == 0) continue;
        Volume volume;
        volume.letter = letter;
        volume.total = total_bytes.QuadPart;
        volume.free = free_bytes.QuadPart;
        volume.writable = ProbeWritable(root);
        volumes.push_back(volume);
        if (volumes.size() >= 16) break;
    }
    (void)local_letter;
    return volumes;
}

} // namespace

int Report(const char* local_dir) {
    const std::string local = local_dir ? local_dir : "";
    g_local_dir = local;
    const char local_letter = LetterOf(local);

    std::printf("UWP storage: LocalState is on %c: (%s)\n", local_letter ? local_letter : '?',
                local.c_str());
    const std::vector<Volume> volumes = QueryVolumes(local_letter);
    if (volumes.empty()) {
        std::puts("UWP storage: no volume answered GetDiskFreeSpaceEx; storage warnings unavailable");
        return 0;
    }
    std::uint64_t local_free = 0, local_total = 0;
    for (const Volume& volume : volumes) {
        std::printf("UWP storage: %c: total=%s free=%s%s%s\n", volume.letter,
                    FormatBytes(volume.total).c_str(), FormatBytes(volume.free).c_str(),
                    volume.letter == local_letter ? " [LocalState]" : "",
                    volume.writable ? " writable" : " read-only to this app");
        if (volume.letter == local_letter) {
            local_free = volume.free;
            local_total = volume.total;
        }
    }

    // Where the space went. The swap file is the one this port creates, and it is the one a player
    // can delete; the dump and the saves are not ours to move.
    if (!local.empty()) {
        FolderSize total;
        const auto folders = WalkFolders(std::filesystem::path(local), total);
        for (const auto& folder : folders) {
            std::printf("UWP storage: LocalState\\%s = %s in %llu file(s)%s\n", folder.name.c_str(),
                        FormatBytes(folder.bytes).c_str(),
                        static_cast<unsigned long long>(folder.files),
                        folder.truncated ? " (walk capped: a lower bound)" : "");
        }
        std::printf("UWP storage: LocalState total = %s in %llu file(s) of %s on the volume\n",
                    FormatBytes(total.bytes).c_str(),
                    static_cast<unsigned long long>(total.files),
                    FormatBytes(local_total).c_str());
    }

    // A bigger volume is not a bigger swap budget unless the app may write there. Say which one is
    // biggest and whether that is where the swap file already is.
    if (const Volume* best = BestWritable(volumes, local_letter)) {
        std::printf("UWP storage: largest writable volume other than LocalState's is %c: with %s "
                    "free (the swap file stays in LocalState unless BB_UWP_SWAP_FILE points "
                    "elsewhere)\n",
                    best->letter, FormatBytes(best->free).c_str());
    } else {
        std::puts("UWP storage: no other writable volume; LocalState's volume is the only place the "
                  "pager can write");
    }

    if (LowStorage(local_free)) {
        std::printf("UWP STORAGE WARNING: %s free on %c:, below the %s this port wants for the swap "
                    "file plus the game's own writes. Paging will be capped to what is left, and the "
                    "run can stall at the memory limit with memory it cannot hand back.\n",
                    FormatBytes(local_free).c_str(), local_letter ? local_letter : '?',
                    FormatBytes(kLowStorageBytes).c_str());
        return 1;
    }
    return 0;
}

std::uint64_t LocalStateFreeBytes() {
    // The volume is derived from the path Report() was given, not re-queried: the app has exactly
    // one LocalState, and asking WinRT again every second would be a package lookup per heartbeat.
    if (g_local_dir.empty()) return 0;
    const char letter = LetterOf(g_local_dir);
    if (!letter) return 0;
    std::wstring root;
    root.push_back((wchar_t)letter);
    root += L":\\";
    ULARGE_INTEGER free_bytes{}, total_bytes{};
    if (!GetDiskFreeSpaceExW(root.c_str(), &free_bytes, &total_bytes, nullptr)) return 0;
    return free_bytes.QuadPart;
}

} // namespace BbStorage
