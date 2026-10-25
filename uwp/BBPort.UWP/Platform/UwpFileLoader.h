// SPDX-License-Identifier: GPL-2.0-or-later
// UwpFileLoader: replaces all desktop Win32 / Tkinter file-picker paths with deterministic
// LocalFolder asset discovery, binary PSF (param.sfo) parsing, and decrypted ELF/SELF validation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace bbport::uwp {

struct SfoMetadata {
    bool valid = false;
    std::string title_id;
    std::string title;
    std::string app_ver;
    std::string version;
    uint32_t attribute = 0;
    std::map<std::string, std::string> strings;
    std::map<std::string, uint32_t> integers;
};

enum class ExecutableFormat {
    Invalid = 0,
    DecryptedElf64,    // 0x7F 'E' 'L' 'F' (x86-64 PS4 ET_DYN/ET_SCE_DYNEXEC)
    DecryptedPs4Self,  // Unencrypted-segment SELF wrapper (0x4F 0x15 0x3D 0x1D)
    EncryptedPs4Self,  // Encrypted SELF (must be decrypted on PS4 before provisioning!)
    PreparedBbProbe,   // Pre-linked BBPROBE5 boot-linked.bin image
};

struct FirstStartAudit {
    bool app0_exists = false;
    bool sfo_valid = false;
    bool is_bloodborne_cusa = false;
    bool is_version_109 = false;
    bool eboot_ready = false;
    bool libc_prx_present = false;
    bool fios2_prx_present = false;
    bool dvdroot_present = false;
    bool boot_linked_present = false;
    ExecutableFormat eboot_format = ExecutableFormat::Invalid;
    SfoMetadata sfo{};
    std::filesystem::path resolved_app0;
    std::vector<std::string> issues;

    bool ReadyToLaunch() const {
        return app0_exists && sfo_valid && is_bloodborne_cusa &&
               (boot_linked_present || (eboot_ready && libc_prx_present && fios2_prx_present)) &&
               dvdroot_present;
    }
};

class UwpFileLoader {
public:
    // Parses a raw PlayStation 4 param.sfo buffer (\0PSF binary format, matching scripts/prepare.py).
    static SfoMetadata ParseParamSfoBytes(const uint8_t* data, size_t size);
    static SfoMetadata ParseParamSfoFile(const std::filesystem::path& sfo_path);

    // Inspects eboot.bin or boot-linked.bin header without loading the whole executable into RAM.
    static ExecutableFormat InspectExecutableHeader(const uint8_t* header_bytes, size_t size);
    static ExecutableFormat InspectExecutableFile(const std::filesystem::path& exe_path);

    // Resolves a PS4 guest path (e.g. "/app0/dvdroot_ps4/parts/..." or "chr/c0000.chrbnd.dcx")
    // safely inside app0_root, rejecting any ".." traversal component.
    static bool ResolveGuestAssetPath(const std::filesystem::path& app0_root,
                                      const std::string& guest_path,
                                      std::filesystem::path* out_host_path);

    // Runs the zero-dialog First-Start Check against LocalFolder (and optional Xbox Dev Mode
    // D:\DevelopmentFiles\Bloodborne\CUSA03173 fallback staging location).
    static FirstStartAudit RunFirstStartCheck(const std::filesystem::path& local_game_dir);
};

} // namespace bbport::uwp
