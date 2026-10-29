// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the OS half of the storage report (storage_report.h holds the testable half).
//
// Every volume is probed the same way: GetDiskFreeSpaceExW for the numbers, then a 64 KiB file that
// is written, flushed and deleted. The probe is what makes the answer real - a UWP app is allowed to
// read most volumes and write almost none of them, and "24 GB free on D:" is worse than useless if
// the swap file cannot live there. The probe file is opened with FILE_FLAG_DELETE_ON_CLOSE so a
// crash cannot leave litter on a player's drive.

#include "storage_report.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fileapifromapp.h>
#include <memoryapi.h>
#include <winioctl.h>

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
    // Never truncate a pre-existing file, even on a drive the app can write.
    const std::wstring path = root + L"bbport-probe-" + std::to_wstring(GetCurrentProcessId()) +
                              L"-" + std::to_wstring(GetTickCount64()) + L".tmp";
    CREATEFILE2_EXTENDED_PARAMETERS params{};
    params.dwSize = sizeof(params);
    params.dwFileAttributes = FILE_ATTRIBUTE_TEMPORARY;
    params.dwFileFlags = FILE_FLAG_DELETE_ON_CLOSE;
    const HANDLE file = CreateFile2FromAppW(path.c_str(), GENERIC_WRITE, 0, CREATE_NEW, &params);
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

// DRIVE_FIXED describes the medium, not the transport: many USB HDDs report DRIVE_FIXED. Query
// the actual device bus as well; when the sandbox denies this query, err on the side of never using
// USB rather than guessing from a letter. This may conservatively exclude D: on some consoles.
VolumeBus QueryVolumeBus(char letter) {
    std::wstring device = L"\\\\.\\";
    device.push_back(static_cast<wchar_t>(letter));
    device += L":";
    CREATEFILE2_EXTENDED_PARAMETERS params{};
    params.dwSize = sizeof(params);
    HANDLE handle = CreateFile2FromAppW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        OPEN_EXISTING, &params);
    if (handle == INVALID_HANDLE_VALUE) return VolumeBus::Unknown;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    alignas(STORAGE_DEVICE_DESCRIPTOR) unsigned char data[1024]{};
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                                    data, sizeof(data), &returned, nullptr);
    CloseHandle(handle);
    if (!ok || returned < offsetof(STORAGE_DEVICE_DESCRIPTOR, BusType) + sizeof(STORAGE_BUS_TYPE))
        return VolumeBus::Unknown;
    const auto* descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(data);
    if (descriptor->RemovableMedia || descriptor->BusType == BusTypeUsb ||
        descriptor->BusType == BusTypeSd || descriptor->BusType == BusTypeMmc)
        return VolumeBus::UsbOrRemovable;
    // Virtual, file-backed, network and unknown buses can hide the underlying transport. Exclude
    // those rather than reporting that a USB-backed virtual disk is safe.
    switch (descriptor->BusType) {
    case BusTypeScsi: case BusTypeAtapi: case BusTypeAta: case BusTypeRAID:
    case BusTypeSas: case BusTypeSata: case BusTypeNvme:
        return VolumeBus::NonUsb;
    default:
        return VolumeBus::Unknown;
    }
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
        switch (GetDriveTypeW(root.c_str())) {
        case DRIVE_FIXED: volume.kind = VolumeKind::Fixed; break;
        case DRIVE_REMOVABLE: volume.kind = VolumeKind::Removable; break;
        case DRIVE_UNKNOWN: case DRIVE_NO_ROOT_DIR: volume.kind = VolumeKind::Unknown; break;
        default: volume.kind = VolumeKind::Other; break;
        }
        volume.bus = volume.kind == VolumeKind::Removable ? VolumeBus::UsbOrRemovable :
                     volume.kind == VolumeKind::Fixed ? QueryVolumeBus(letter) : VolumeBus::Unknown;
        volumes.push_back(volume);
        if (volumes.size() >= 16) break;
    }
    (void)local_letter;
    return volumes;
}

} // namespace

static const char* KindName(VolumeKind kind) {
    switch (kind) {
    case VolumeKind::Fixed: return "fixed";
    case VolumeKind::Removable: return "removable";
    case VolumeKind::Other: return "other (not eligible)";
    default: return "unknown (not eligible)";
    }
}

static const char* BusName(VolumeBus bus) {
    switch (bus) {
    case VolumeBus::NonUsb: return "verified non-USB";
    case VolumeBus::UsbOrRemovable: return "USB/removable";
    default: return "unknown (not eligible)";
    }
}

// Parent directory with a trailing separator, or empty for a relative/UNC path. Requiring an
// absolute drive path makes removable-device validation unambiguous and avoids clobbering an
// unrelated file through a relative path.
static std::string SwapParent(const std::string& path) {
    if (path.size() < 4 || !LetterOf(path) || (path[2] != '\\' && path[2] != '/')) return {};
    const auto at = path.find_last_of("\\/");
    if (at == std::string::npos || at + 1 >= path.size()) return {};
    return path.substr(0, at + 1);
}

static std::string UniqueSwapPath(const std::string& parent) {
    return parent + "bloodborne-swap-" + std::to_string(GetCurrentProcessId()) + "-" +
           std::to_string(GetTickCount64()) + ".bin";
}

SwapChoice ChooseSwap(const std::string& local_dir, const std::string& requested_path,
                      const char* requested_source, bool auto_select, std::uint64_t reserve) {
    const char local_letter = LetterOf(local_dir);
    const std::string local_parent = local_dir + (local_dir.empty() || local_dir.back() == '\\' ? "" : "\\");
    const auto volumes = QueryVolumes(local_letter);
    auto free_of = [&](char letter) -> std::uint64_t {
        for (const auto& volume : volumes) if (volume.letter == letter) return volume.free;
        ULARGE_INTEGER available{};
        const auto wide = Wide(local_parent);
        return letter == local_letter && !wide.empty() &&
                       GetDiskFreeSpaceExW(wide.c_str(), &available, nullptr, nullptr)
                   ? available.QuadPart : 0;
    };
    if (!requested_path.empty()) {
        const auto parent = SwapParent(requested_path);
        const char letter = LetterOf(requested_path);
        bool eligible = letter == local_letter && letter != 0;
        if (!eligible) {
            for (const auto& v : volumes) {
                if (v.letter == letter && v.kind == VolumeKind::Fixed &&
                    v.bus == VolumeBus::NonUsb) eligible = true;
            }
        }
        const auto wide_path = Wide(requested_path);
        WIN32_FILE_ATTRIBUTE_DATA attr{};
        const bool exists = !wide_path.empty() &&
            GetFileAttributesExFromAppW(wide_path.c_str(), GetFileExInfoStandard, &attr);
        const DWORD attr_error = exists ? ERROR_SUCCESS : GetLastError();
        const bool definitely_missing = !exists &&
            (attr_error == ERROR_FILE_NOT_FOUND || attr_error == ERROR_PATH_NOT_FOUND);
        if (!parent.empty() && eligible && definitely_missing && ProbeWritable(Wide(parent))) {
            return {requested_path, requested_source ? requested_source : "explicit", letter,
                    free_of(letter), letter == local_letter};
        }
        std::printf("UWP swap: requested path rejected (relative, removable/unknown, existing, or "
                    "not writable); no existing file will be overwritten: %s\n", requested_path.c_str());
    }
    if (auto_select) {
        if (const Volume* best = BestSwapVolume(volumes, local_letter, reserve)) {
            const std::string parent = std::string(1, best->letter) + ":\\";
            return {UniqueSwapPath(parent), "automatic", best->letter, best->free, false};
        }
        std::puts("UWP swap: no writable, non-removable alternative with space above the reserve; "
                  "falling back to LocalState");
    }
    return {UniqueSwapPath(local_parent), auto_select ? "LocalState fallback" : "LocalState (auto off)",
            local_letter, free_of(local_letter), true};
}

void TestSwapSpeed(const SwapChoice& choice, std::uint64_t reserve, bool enabled) {
    constexpr std::uint64_t total = std::uint64_t{64} << 20;
    constexpr DWORD block_size = 1024 * 1024;
    if (!enabled) {
        std::puts("UWP swap speed: 64 MiB test disabled (swap_speed_test=0)");
        return;
    }
    if (choice.path.empty() || choice.free <= reserve || choice.free - reserve < total) {
        std::puts("UWP swap speed: 64 MiB test skipped; not enough free space above reserve");
        return;
    }
    const std::string parent = SwapParent(choice.path);
    if (parent.empty()) {
        std::puts("UWP swap speed: invalid path; test skipped");
        return;
    }
    const std::wstring path = Wide(parent + "bbport-speed-" + std::to_string(GetCurrentProcessId()) +
                                   "-" + std::to_string(GetTickCount64()) + ".tmp");
    CREATEFILE2_EXTENDED_PARAMETERS params{};
    params.dwSize = sizeof(params);
    params.dwFileAttributes = FILE_ATTRIBUTE_TEMPORARY;
    params.dwFileFlags = FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_WRITE_THROUGH |
                         FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN;
    HANDLE file = CreateFile2FromAppW(path.c_str(), GENERIC_WRITE | GENERIC_READ, 0, CREATE_NEW, &params);
    if (file == INVALID_HANDLE_VALUE) {
        std::printf("UWP swap speed: 64 MiB test could not create temporary file (error %lu)\n", GetLastError());
        return;
    }
    auto* block = static_cast<unsigned char*>(
        VirtualAllocFromApp(nullptr, block_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!block) {
        std::printf("UWP swap speed: aligned 1 MiB buffer unavailable (error %lu)\n", GetLastError());
        CloseHandle(file);
        return;
    }
    // Incompressible-ish test data, not a sparse file of zeros. VirtualAlloc is page aligned for
    // unbuffered I/O and every transfer size and offset is a multiple of 1 MiB.
    std::uint32_t state = 0x12345678;
    for (DWORD i = 0; i < block_size; ++i) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        block[i] = static_cast<unsigned char>(state);
    }
    const auto start = std::chrono::steady_clock::now();
    bool ok = true;
    DWORD done = 0;
    for (unsigned i = 0; i < 64 && ok; ++i)
        ok = WriteFile(file, block, block_size, &done, nullptr) && done == block_size;
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    const auto written = std::chrono::steady_clock::now();
    LARGE_INTEGER zero{};
    if (ok) ok = SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) != FALSE;
    for (unsigned i = 0; i < 64 && ok; ++i)
        ok = ReadFile(file, block, block_size, &done, nullptr) && done == block_size;
    const auto read = std::chrono::steady_clock::now();
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (ok) {
        const double write_s = std::chrono::duration<double>(written - start).count();
        const double read_s = std::chrono::duration<double>(read - written).count();
        std::printf("UWP swap speed: 64 MiB %c: unbuffered write %.1f MiB/s (%.0f ms), "
                    "read %.1f MiB/s (%.0f ms)\n", choice.letter,
                    write_s > 0 ? 64.0 / write_s : 0, write_s * 1000,
                    read_s > 0 ? 64.0 / read_s : 0, read_s * 1000);
    } else {
        std::printf("UWP swap speed: 64 MiB test incomplete (error %lu)\n", error);
    }
    CloseHandle(file);
    VirtualFree(block, 0, MEM_RELEASE);
    std::fflush(stdout);
}

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
        std::printf("UWP storage: %c: total=%s free=%s%s%s type=%s bus=%s\n", volume.letter,
                    FormatBytes(volume.total).c_str(), FormatBytes(volume.free).c_str(),
                    volume.letter == local_letter ? " [LocalState]" : "",
                    volume.writable ? " writable" : " read-only to this app", KindName(volume.kind), BusName(volume.bus));
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

    if (const Volume* best = BestSwapVolume(volumes, local_letter, std::uint64_t{384} << 20)) {
        std::printf("UWP storage: largest eligible fixed volume other than LocalState is %c: "
                    "with %s free (swap path printed in the run header)\n",
                    best->letter, FormatBytes(best->free).c_str());
    } else {
        std::puts("UWP storage: no other writable fixed volume confirmed; removable and unknown "
                  "volumes are excluded from automatic swap");
    }

    if (LowStorage(local_free)) {
        std::printf("UWP STORAGE WARNING: %s free on %c:, below the %s this port wants for the swap "
                    "for the game's own writes. The chosen swap volume is reported separately.\n",
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
