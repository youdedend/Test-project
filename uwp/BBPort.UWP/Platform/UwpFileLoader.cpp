// SPDX-License-Identifier: GPL-2.0-or-later
#include "UwpFileLoader.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <system_error>

namespace bbport::uwp {

namespace {

uint16_t ReadLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool IsSupportedBloodborneTitleId(const std::string& id) {
    static constexpr const char* kIds[] = {
        "CUSA03173", // GOTY EU (primary recommended target)
        "CUSA00900", // US
        "CUSA00207", // EU
        "CUSA00208", // UK
        "CUSA00299", // Asia
        "CUSA01363", // JP
        "CUSA03014", // Old Hunters JP
        "CUSA03023", // Old Hunters Asia
        "CUSA03179", // GOTY UK
    };
    for (const char* known : kIds) {
        if (id == known) return true;
    }
    return false;
}

} // namespace

SfoMetadata UwpFileLoader::ParseParamSfoBytes(const uint8_t* data, size_t size) {
    SfoMetadata meta{};
    if (!data || size < 20) return meta;
    // Magic: "\0PSF" (0x00, 'P', 'S', 'F')
    if (data[0] != 0x00 || data[1] != 'P' || data[2] != 'S' || data[3] != 'F') {
        return meta;
    }

    const uint32_t key_table_off = ReadLe32(data + 8);
    const uint32_t val_table_off = ReadLe32(data + 12);
    const uint32_t count         = ReadLe32(data + 16);

    if (count > 512 || 20ull + static_cast<uint64_t>(count) * 16ull > size ||
        key_table_off >= size || val_table_off > size) {
        return meta;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* entry = data + 20 + i * 16;
        const uint16_t key_off = ReadLe16(entry + 0);
        const uint16_t fmt     = ReadLe16(entry + 2);
        const uint32_t val_len = ReadLe32(entry + 4);
        const uint32_t val_off = ReadLe32(entry + 12);

        const size_t k_pos = static_cast<size_t>(key_table_off) + key_off;
        if (k_pos >= size) return SfoMetadata{};
        const void* nul = std::memchr(data + k_pos, 0, size - k_pos);
        if (!nul) return SfoMetadata{};
        const std::string key(reinterpret_cast<const char*>(data + k_pos),
                              static_cast<const uint8_t*>(nul) - (data + k_pos));

        const uint64_t v_pos = static_cast<uint64_t>(val_table_off) + val_off;
        if (v_pos + val_len > size) return SfoMetadata{};

        if (fmt == 0x0404u && val_len == 4) {
            const uint32_t v = ReadLe32(data + v_pos);
            meta.integers[key] = v;
        } else if (fmt == 0x0204u || fmt == 0x0004u) {
            std::string s(reinterpret_cast<const char*>(data + v_pos), val_len);
            while (!s.empty() && s.back() == '\0') s.pop_back();
            meta.strings[key] = s;
        }
    }

    meta.valid    = true;
    meta.title_id = meta.strings["TITLE_ID"];
    meta.title    = meta.strings["TITLE"];
    meta.app_ver  = meta.strings["APP_VER"];
    meta.version  = meta.strings["VERSION"];
    if (auto it = meta.integers.find("ATTRIBUTE"); it != meta.integers.end()) {
        meta.attribute = it->second;
    }
    return meta;
}

SfoMetadata UwpFileLoader::ParseParamSfoFile(const std::filesystem::path& sfo_path) {
    std::ifstream ifs(sfo_path, std::ios::binary);
    if (!ifs) return {};
    ifs.seekg(0, std::ios::end);
    const auto sz = ifs.tellg();
    if (sz <= 0 || sz > 1024 * 1024) return {};
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    ifs.read(reinterpret_cast<char*>(buf.data()), sz);
    return ParseParamSfoBytes(buf.data(), buf.size());
}

ExecutableFormat UwpFileLoader::InspectExecutableHeader(const uint8_t* h, size_t size) {
    if (!h || size < 16) return ExecutableFormat::Invalid;

    // 1. Pre-prepared BBPROBE1..5 boot image
    if (std::memcmp(h, "BBPROBE", 7) == 0 && h[7] >= '1' && h[7] <= '5') {
        return ExecutableFormat::PreparedBbProbe;
    }

    // 2. Plain decrypted ELF64 x86-64 (Itemzflow / decrypted dump)
    if (size >= 64 && h[0] == 0x7F && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') {
        const uint8_t elf_class = h[4];    // 2 = ELFCLASS64
        const uint8_t elf_data  = h[5];    // 1 = ELFDATA2LSB
        const uint16_t machine  = ReadLe16(h + 18); // 62 = EM_X86_64
        if (elf_class == 2 && elf_data == 1 && machine == 62) {
            return ExecutableFormat::DecryptedElf64;
        }
        return ExecutableFormat::Invalid;
    }

    // 3. PS4 SELF header (magic 0x4F 0x15 0x3D 0x1D)
    if (size >= 32 && h[0] == 0x4F && h[1] == 0x15 && h[2] == 0x3D && h[3] == 0x1D) {
        const uint16_t num_entries = ReadLe16(h + 24);
        const size_t elf_hdr_off = 32u + static_cast<size_t>(num_entries) * 32u;
        if (elf_hdr_off + 64 <= size &&
            h[elf_hdr_off + 0] == 0x7F && h[elf_hdr_off + 1] == 'E' &&
            h[elf_hdr_off + 2] == 'L'  && h[elf_hdr_off + 3] == 'F') {
            // Check whether any SELF segment entry still has encrypted flag bit (0x2 or 0x4 in props)
            for (uint16_t i = 0; i < num_entries; ++i) {
                const uint64_t props = ReadLe32(h + 32 + i * 32);
                if (props & 0x2u) {
                    return ExecutableFormat::EncryptedPs4Self;
                }
            }
            return ExecutableFormat::DecryptedPs4Self;
        }
        return ExecutableFormat::EncryptedPs4Self;
    }

    return ExecutableFormat::Invalid;
}

ExecutableFormat UwpFileLoader::InspectExecutableFile(const std::filesystem::path& exe_path) {
    std::ifstream ifs(exe_path, std::ios::binary);
    if (!ifs) return ExecutableFormat::Invalid;
    std::array<uint8_t, 4096> header{};
    ifs.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    const auto got = static_cast<size_t>(ifs.gcount());
    return InspectExecutableHeader(header.data(), got);
}

bool UwpFileLoader::ResolveGuestAssetPath(const std::filesystem::path& app0_root,
                                          const std::string& guest_path,
                                          std::filesystem::path* out_host_path) {
    if (guest_path.empty() || !out_host_path) return false;

    std::string rel = guest_path;
    std::replace(rel.begin(), rel.end(), '\\', '/');

    if (rel.rfind("/app0/", 0) == 0) {
        rel.erase(0, 6);
    } else if (rel == "/app0") {
        rel.clear();
    } else if (rel.rfind("/hostapp/", 0) == 0) {
        rel.erase(0, 9);
    } else if (!rel.empty() && rel.front() == '/') {
        return false; // Only /app0 and /hostapp resolve against game assets
    }

    // Reject any ".." segment strictly (matching src/runtime_file.c security invariant).
    size_t pos = 0;
    while (pos <= rel.size()) {
        const size_t slash = rel.find('/', pos);
        const std::string_view part = (slash == std::string::npos)
            ? std::string_view(rel).substr(pos)
            : std::string_view(rel).substr(pos, slash - pos);
        if (part == "..") {
            return false;
        }
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }

    *out_host_path = rel.empty() ? app0_root : (app0_root / rel);
    return true;
}

FirstStartAudit UwpFileLoader::RunFirstStartCheck(const std::filesystem::path& local_game_dir) {
    FirstStartAudit audit{};
    std::error_code ec;

    // Candidate 1: ApplicationData.Current.LocalFolder/game/CUSA03173 (canonical provisioned location)
    // Candidate 2: Xbox Dev Mode D:\DevelopmentFiles\Bloodborne\CUSA03173 (SFTP/SMB staging folder)
    std::filesystem::path candidate = local_game_dir;
    if (!std::filesystem::exists(candidate / "sce_sys" / "param.sfo", ec)) {
        const std::filesystem::path dev_files("D:/DevelopmentFiles/Bloodborne/CUSA03173");
        if (std::filesystem::exists(dev_files / "sce_sys" / "param.sfo", ec)) {
            candidate = dev_files;
        }
    }

    audit.resolved_app0 = candidate;
    audit.app0_exists = std::filesystem::is_directory(candidate, ec);
    if (!audit.app0_exists) {
        audit.issues.push_back("Game folder LocalState\\game\\CUSA03173 is missing. Run uwp\\provision.ps1 or transfer via SFTP.");
        return audit;
    }

    const auto sfo_path = candidate / "sce_sys" / "param.sfo";
    audit.sfo = ParseParamSfoFile(sfo_path);
    audit.sfo_valid = audit.sfo.valid;
    if (!audit.sfo_valid) {
        audit.issues.push_back("Missing or invalid sce_sys\\param.sfo inside CUSA03173.");
    } else {
        audit.is_bloodborne_cusa = IsSupportedBloodborneTitleId(audit.sfo.title_id);
        audit.is_version_109 = (audit.sfo.app_ver == "01.09" || audit.sfo.version == "01.09");
        if (!audit.is_bloodborne_cusa) {
            audit.issues.push_back("Unsupported TITLE_ID '" + audit.sfo.title_id + "' (expected CUSA03173).");
        }
        if (!audit.is_version_109) {
            audit.issues.push_back("Game update APP_VER is '" + audit.sfo.app_ver +
                                   "' (version 01.09 is required for 60 FPS and resolution patches).");
        }
    }

    const auto boot_linked = candidate / "boot-linked.bin";
    audit.boot_linked_present = std::filesystem::is_regular_file(boot_linked, ec) &&
                                InspectExecutableFile(boot_linked) == ExecutableFormat::PreparedBbProbe;

    const auto eboot_path = candidate / "eboot.bin";
    audit.eboot_format = InspectExecutableFile(eboot_path);
    audit.eboot_ready = (audit.eboot_format == ExecutableFormat::DecryptedElf64 ||
                         audit.eboot_format == ExecutableFormat::DecryptedPs4Self);
    if (audit.eboot_format == ExecutableFormat::EncryptedPs4Self) {
        audit.issues.push_back("eboot.bin is still an encrypted PS4 SELF; dump a decrypted copy from your PS4.");
    } else if (!audit.eboot_ready && !audit.boot_linked_present) {
        audit.issues.push_back("Missing decrypted eboot.bin (or pre-linked boot-linked.bin).");
    }

    audit.libc_prx_present  = std::filesystem::is_regular_file(candidate / "sce_module" / "libc.prx", ec);
    audit.fios2_prx_present = std::filesystem::is_regular_file(candidate / "sce_module" / "libSceFios2.prx", ec);
    audit.dvdroot_present   = std::filesystem::is_directory(candidate / "dvdroot_ps4", ec);

    if (!audit.libc_prx_present && !audit.boot_linked_present) {
        audit.issues.push_back("Missing sce_module\\libc.prx.");
    }
    if (!audit.fios2_prx_present && !audit.boot_linked_present) {
        audit.issues.push_back("Missing sce_module\\libSceFios2.prx.");
    }
    if (!audit.dvdroot_present) {
        audit.issues.push_back("Missing dvdroot_ps4 asset directory.");
    }

    return audit;
}

} // namespace bbport::uwp
