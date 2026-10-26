// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Native C++ game preparation for Bloodborne on Xbox Series X|S Dev Mode UWP.
// Produces byte-for-byte identical outputs to scripts/prepare.py, scripts/link_modules.py,
// scripts/content_profile.py, and scripts/patches.py from the player's own decrypted PS4 dump.
namespace BbPrepare {

using Bytes = std::vector<uint8_t>;

struct Dump {
    Bytes eboot;
    Bytes libc;
    Bytes fios;
    Bytes sfo;
};

/// The software metadata read out of a dump's sce_sys/param.sfo.
struct SfoInfo {
    std::string app_ver;    // APP_VER, e.g. "01.09" ("" when the key is absent)
    std::string app_type;   // APP_TYPE, "gp" for a base game, "patch" (or "gd") for an update
    std::string title_id;   // TITLE_ID, e.g. "CUSA03173"
    std::string content_id; // CONTENT_ID, the update version is encoded in it
    std::string path;       // the param.sfo this came from ("" when parsed from bytes)
    std::string update_ver; // version decoded from CONTENT_ID (e.g. "01.09"), may be empty

    /// True when this param.sfo belongs to an update/patch container rather than a base game.
    bool IsPatch() const;
    /// The best version this container can be credited with: APP_VER, else the CONTENT_ID group.
    const std::string& Version() const;
};

/// Parses param.sfo bytes. Returns nullopt when the file is not a valid PSF or has no APP_VER;
/// the raw header bytes are logged so a broken dump can be diagnosed from loader.log.
std::optional<SfoInfo> ParseParamSfo(const Bytes& sfo, const std::string& path);
/// Reads and parses `<dir>\sce_sys\param.sfo`. Never falls back to a default version.
std::optional<SfoInfo> ReadParamSfo(const std::string& dir);

struct BootPrepared {
    Bytes boot_linked; // BBPROBE5 (eboot + libc.prx + libSceFios2.prx + redzone trampolines)
    Bytes content;     // BBCONT01 (28-byte offline AppContent profile)
    Bytes eboot_elf;   // Unpacked ELF image for fast startup patch compilation
    std::string title_id;
    std::string title;
    std::string app_ver;
};

struct PatchBuildResult {
    Bytes patches_bin; // BBPATCH2
    int render_width = 1920;
    int render_height = 1080;
    std::string summary;
};

/// How a dump folder is laid out on disk, and which file sources the app will actually run.
struct DumpLayout {
    enum class Kind {
        Merged,   ///< one folder that already holds the highest version (patched in place)
        Split,    ///< base game folder plus a separate update/patch folder
        BaseOnly, ///< base game only: anything the patch set targets cannot be applied
    };

    Kind kind = Kind::BaseOnly;
    std::string base_path;      ///< folder with the base game data (eboot/sce_module/sce_sys)
    std::string patch_path;     ///< update container folder (empty unless Kind::Split)
    SfoInfo base;               ///< param.sfo of `base_path`
    SfoInfo patch;              ///< param.sfo of `patch_path` (empty unless Kind::Split)
    std::string effective_ver;  ///< version the app will run (patch version when split)

    [[nodiscard]] bool IsValid() const {
        return !base.app_ver.empty();
    }
    /// "merged" / "split" / "base-only".
    [[nodiscard]] const char* KindName() const;
    /// Player-facing multi-line summary for the launcher panel.
    [[nodiscard]] std::string Summary() const;
    /// The single `DumpLayout: ...` line written to loader.log.
    [[nodiscard]] std::string LogLine() const;
};

/// Detects the layout of `user_picked`:
///  - a param.sfo that belongs to an update (APP_TYPE patch/gd or a CONTENT_ID version) is used
///    directly, and a sibling base-game folder is picked up as its data source when present;
///  - a param.sfo at the target version is a merged, already-updated dump;
///  - otherwise the parent folder, the folder itself and `preferred_patch` (bbport.ini
///    [dump] patch_path) are searched for the highest-version update container with the same
///    TITLE_ID. Without one the layout is BaseOnly.
/// Logs exactly one `DumpLayout: ...` line.
DumpLayout DetectDumpLayout(const std::string& user_picked, const std::string& preferred_patch = {});

// Empty string when param.sfo names a retail Bloodborne release, otherwise a player-facing error.
std::string CheckTitle(const Bytes& sfo, std::string* out_title_id = nullptr,
                       std::string* out_app_ver = nullptr);

// Fingerprint over the dump inputs and redzone.json so Prepared/boot-linked.bin is cached safely.
uint64_t Fingerprint(const Dump& dump, const std::string& redzone_json);

// Prepares boot-linked.bin (BBPROBE5) and content.bin (BBCONT01). Throws std::runtime_error on bad input.
BootPrepared PrepareBoot(const Dump& dump, const std::string& redzone_json,
                         bool enable_red_zones = true);

// Compiles patches.bin (BBPATCH2) from patches/Bloodborne.xml + bbport.ini settings + unpacked eboot ELF.
PatchBuildResult BuildPatches(const Bytes& eboot_elf, const std::string& app_ver,
                              const std::string& patch_xml,
                              const std::map<std::string, std::string>& settings,
                              const std::string& fps_mode = "60",
                              const std::string& render_res_override = "",
                              bool force_patches = false);

} // namespace BbPrepare
