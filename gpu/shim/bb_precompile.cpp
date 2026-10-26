// SPDX-License-Identifier: GPL-2.0-or-later
// Xbox UWP shader cache sync, Bloodborne .shaderbnd.dcx OrbShdr verifier, and pre-game warm-up UI.

#include "bb_precompile.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unordered_set>
#include <vector>

#include <miniz.h>

#include "bbport_overlay.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

#ifdef BB_UWP
#include <windows.h>
#include <malloc.h>
#endif

namespace BbPrecompile {

namespace {

void CompactHostHeaps() {
#ifdef BB_UWP
    _heapmin();
    if (const HANDLE heap = GetProcessHeap()) {
        HeapCompact(heap, 0);
    }
#endif
}

u32 ReadBe32(const u8* p) {
    return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

u32 ReadLe32(const u8* p) {
    u32 v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

u64 ReadLe64(const u8* p) {
    u64 v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

bool CopyFileViaIoFile(const std::filesystem::path& src, const std::filesystem::path& dst) {
    Common::FS::IOFile in{src, Common::FS::FileAccessMode::Read};
    if (!in.IsOpen()) {
        return false;
    }
    const u64 size = in.GetSize();
    if (size == 0 || size > 64ull * 1024 * 1024) {
        return false;
    }
    if (std::filesystem::exists(dst)) {
        std::error_code ec;
        if (std::filesystem::file_size(dst, ec) == size && !ec) {
            return false;
        }
    }
    std::vector<u8> buf(static_cast<size_t>(size));
    if (in.ReadRaw<u8>(buf.data(), buf.size()) != buf.size()) {
        return false;
    }
    in.Close();

    std::error_code ec;
    std::filesystem::create_directories(dst.parent_path(), ec);
    auto tmp = dst;
    tmp += ".sync.tmp";
    {
        Common::FS::IOFile out{tmp, Common::FS::FileAccessMode::Create};
        if (!out.IsOpen() || out.WriteRaw<u8>(buf.data(), buf.size()) != buf.size()) {
            out.Close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::remove(dst, ec);
    ec.clear();
    std::filesystem::rename(tmp, dst, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

u32 SyncDirectoryTree(const std::filesystem::path& src_root,
                      const std::filesystem::path& dst_root,
                      const ProgressFn& progress,
                      const char* label_prefix) {
    std::error_code ec;
    if (src_root.empty() || dst_root.empty() || !std::filesystem::exists(src_root, ec)) {
        return 0;
    }
    std::vector<std::filesystem::path> rel_files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator{src_root, ec}) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const auto ext = entry.path().extension().string();
        if (ext == ".spv" || ext == ".meta" || ext == ".key" || ext == ".bin" ||
            ext == ".vkcache") {
            const auto rel = std::filesystem::relative(entry.path(), src_root, ec);
            if (!ec && !rel.empty()) {
                rel_files.push_back(rel);
            }
        }
    }
    if (rel_files.empty()) {
        return 0;
    }
    u32 copied = 0;
    const u32 total = static_cast<u32>(rel_files.size());
    for (u32 i = 0; i < total; ++i) {
        if (CopyFileViaIoFile(src_root / rel_files[i], dst_root / rel_files[i])) {
            ++copied;
        }
        if (progress && ((i % 16u) == 0u || i + 1 == total)) {
            progress(label_prefix, i + 1, total);
        }
    }
    return copied;
}

std::filesystem::path LocalCacheRoot() {
    const auto serial = Common::ElfInfo::Instance().GameSerial();
    if (serial.empty()) {
        return {};
    }
    return Common::FS::GetUserPath(Common::FS::PathType::CacheDir) / serial;
}

std::filesystem::path LocalPortableCacheRoot() {
    const auto serial = Common::ElfInfo::Instance().GameSerial();
    const char* user_dir = std::getenv("BB_GPU_USER_DIR");
    if (serial.empty() || !user_dir || !*user_dir) {
        return {};
    }
    // BB_GPU_USER_DIR is <LocalState>/user; place portable cache at <LocalState>/shader_cache/<serial>.
    return std::filesystem::path{user_dir}.parent_path() / "shader_cache" / serial;
}

std::filesystem::path DumpPortableCacheRoot() {
    const auto serial = Common::ElfInfo::Instance().GameSerial();
    const char* dump = std::getenv("BB_UWP_DUMP");
    if (serial.empty() || !dump || !*dump) {
        return {};
    }
    return std::filesystem::path{dump} / "shader_cache" / serial;
}

bool DecompressDcxDflt(const std::filesystem::path& path, std::vector<u8>& out_data) {
    out_data.clear();
    Common::FS::IOFile file{path, Common::FS::FileAccessMode::Read};
    if (!file.IsOpen()) {
        return false;
    }
    const u64 file_size = file.GetSize();
    if (file_size < 0x4C || file_size > 64ull * 1024 * 1024) {
        return false;
    }
    std::vector<u8> raw(static_cast<size_t>(file_size));
    if (file.ReadRaw<u8>(raw.data(), raw.size()) != raw.size()) {
        return false;
    }
    file.Close();

    if (std::memcmp(raw.data(), "DCX\0", 4) != 0) {
        return false;
    }
    const u32 dcs_off = ReadBe32(raw.data() + 0x08);
    const u32 dcp_off = ReadBe32(raw.data() + 0x0C);
    if (dcs_off + 12 > raw.size() || dcp_off + 0x20 > raw.size()) {
        return false;
    }
    if (std::memcmp(raw.data() + dcs_off, "DCS\0", 4) != 0 ||
        std::memcmp(raw.data() + dcp_off, "DCP\0", 4) != 0 ||
        std::memcmp(raw.data() + dcp_off + 4, "DFLT", 4) != 0) {
        return false;
    }
    const u32 uncomp_size = ReadBe32(raw.data() + dcs_off + 4);
    const u32 comp_size = ReadBe32(raw.data() + dcs_off + 8);
    if (uncomp_size == 0 || uncomp_size > 96u * 1024 * 1024 || comp_size == 0) {
        return false;
    }

    size_t zlib_off = 0;
    const size_t scan_limit = std::min<size_t>(raw.size() - 8, 256);
    for (size_t pos = dcp_off + 8; pos <= scan_limit; ++pos) {
        if (std::memcmp(raw.data() + pos, "DCA\0", 4) == 0) {
            const u32 dca_len = ReadBe32(raw.data() + pos + 4);
            if (dca_len >= 8 && pos + dca_len < raw.size()) {
                zlib_off = pos + dca_len;
                break;
            }
        }
    }
    if (zlib_off == 0 || zlib_off >= raw.size()) {
        return false;
    }
    const size_t avail_comp = std::min<size_t>(comp_size, raw.size() - zlib_off);
    out_data.resize(uncomp_size);
    mz_ulong dest_len = static_cast<mz_ulong>(uncomp_size);
    const int rc = mz_uncompress(out_data.data(), &dest_len, raw.data() + zlib_off,
                                 static_cast<mz_ulong>(avail_comp));
    if (rc != MZ_OK || dest_len != uncomp_size) {
        out_data.clear();
        return false;
    }
    return true;
}

} // namespace

u32 SyncPortableCacheToLocal(const ProgressFn& progress) {
    const auto local = LocalCacheRoot();
    if (local.empty()) {
        return 0;
    }
    u32 imported = 0;
    imported += SyncDirectoryTree(DumpPortableCacheRoot(), local, progress,
                                  "Importing USB/dump shader cache...");
    imported += SyncDirectoryTree(LocalPortableCacheRoot(), local, progress,
                                  "Importing LocalState portable shader cache...");
    if (imported > 0) {
        std::printf("GPU: imported %u portable shader/pipeline cache files into %s\n", imported,
                    local.string().c_str());
    }
    return imported;
}

u32 ExportLocalCacheToPortable() {
    const auto local = LocalCacheRoot();
    const auto portable = LocalPortableCacheRoot();
    if (local.empty() || portable.empty()) {
        return 0;
    }
    const u32 exported = SyncDirectoryTree(local, portable, {}, "Exporting shader cache...");
    if (exported > 0) {
        std::printf("GPU: exported %u shader/pipeline cache files to %s\n", exported,
                    portable.string().c_str());
    }
    return exported;
}

ScanSummary ScanGameShaderArchives(const ProgressFn& progress) {
    ScanSummary summary{};
    const char* dump = std::getenv("BB_UWP_DUMP");
    if (!dump || !*dump) {
        return summary;
    }
    const std::filesystem::path shader_dir =
        std::filesystem::path{dump} / "dvdroot_ps4" / "shader";

    static constexpr std::array<const char*, 10> kKnownArchives = {
        "allmaterialbnd.shaderbnd.dcx",
        "sfxshaderbnd.shaderbnd.dcx",
        "MenuShader.shaderbnd.dcx",
        "menushader.shaderbnd.dcx",
        "DebugShader.shaderbnd.dcx",
        "debugshader.shaderbnd.dcx",
        "FilterShader.shaderbnd.dcx",
        "filtershader.shaderbnd.dcx",
        "GXShader.shaderbnd.dcx",
        "gxshader.shaderbnd.dcx",
    };

    std::vector<std::filesystem::path> archives;
    std::unordered_set<std::string> seen_names;
    std::error_code ec;
    if (std::filesystem::exists(shader_dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator{shader_dir, ec}) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            std::string fname = entry.path().filename().string();
            std::string lower = fname;
            std::ranges::transform(lower, lower.begin(),
                                   [](unsigned char c) { return char(std::tolower(c)); });
            if (lower.ends_with(".shaderbnd.dcx") && seen_names.insert(lower).second) {
                archives.push_back(entry.path());
            }
        }
    }
    if (archives.empty()) {
        // USB FutureAccessList paths may block directory_iterator while allowing direct file open.
        for (const char* name : kKnownArchives) {
            std::string lower = name;
            std::ranges::transform(lower, lower.begin(),
                                   [](unsigned char c) { return char(std::tolower(c)); });
            if (seen_names.contains(lower)) {
                continue;
            }
            const auto candidate = shader_dir / name;
            Common::FS::IOFile probe{candidate, Common::FS::FileAccessMode::Read};
            if (probe.IsOpen()) {
                probe.Close();
                seen_names.insert(lower);
                archives.push_back(candidate);
            }
        }
    }

    summary.archives_found = static_cast<u32>(archives.size());
    if (archives.empty()) {
        std::printf("GPU: no .shaderbnd.dcx archives found under %s\n",
                    shader_dir.string().c_str());
        return summary;
    }

    std::unordered_set<u64> unique_hashes;
    for (u32 idx = 0; idx < summary.archives_found; ++idx) {
        const auto& path = archives[idx];
        if (progress) {
            progress("Scanning " + path.filename().string() + "...", idx, summary.archives_found);
        }
        std::vector<u8> bnd;
        if (!DecompressDcxDflt(path, bnd)) {
            std::printf("GPU: failed to decompress %s\n", path.filename().string().c_str());
            continue;
        }
        ++summary.archives_ok;

        if (bnd.size() >= 28) {
            const size_t limit = bnd.size() - 28;
            for (size_t off = 0; off <= limit; off += 4) {
                if (bnd[off] != 'O' || std::memcmp(bnd.data() + off, "OrbShdr", 7) != 0) {
                    continue;
                }
                const u32 word2 = ReadLe32(bnd.data() + off + 8);
                const u32 code_len = word2 >> 8;
                const u32 shdr_type = (word2 >> 2) & 0xFu;
                if (code_len == 0 || (code_len & 3u) != 0 || code_len > off) {
                    continue;
                }
                const u64 hash = ReadLe64(bnd.data() + off + 16);
                ++summary.total_orbshdr;
                unique_hashes.insert(hash ? hash : (off ^ (u64(idx) << 32)));
                switch (shdr_type) {
                case 0: // PS
                    ++summary.ps_count;
                    break;
                case 1: // VS
                    ++summary.vs_count;
                    break;
                case 3: // CS
                    ++summary.cs_count;
                    break;
                default:
                    ++summary.gs_hs_ds_count;
                    break;
                }
            }
        }

        // Release decompressed BND buffer immediately before moving to the next archive.
        std::vector<u8>{}.swap(bnd);
        CompactHostHeaps();
    }

    summary.unique_hashes = static_cast<u32>(unique_hashes.size());
    if (progress) {
        progress("Verified Bloodborne shader archives", summary.archives_found,
                 summary.archives_found);
    }
    std::printf("GPU: verified %u/%u .shaderbnd.dcx archives: %u OrbShdr binaries "
                "(%u unique: VS=%u PS=%u CS=%u GS/HS/DS=%u)\n",
                summary.archives_ok, summary.archives_found, summary.total_orbshdr,
                summary.unique_hashes, summary.vs_count, summary.ps_count, summary.cs_count,
                summary.gs_hs_ds_count);
    std::fflush(stdout);
    return summary;
}

void WarmUpBeforeBoot(Vulkan::Rasterizer& rasterizer,
                      const std::function<void()>& present_frame) {
    const char* warm_env = std::getenv("BB_WARMUP");
    if (warm_env && *warm_env == '0') {
        return;
    }

    auto ui_progress = [&](const std::string& label, u32 done, u32 total) {
        BbOverlay::SetLoadingLabel(label);
        BbOverlay::SetLoading(true, done, std::max(total, 1u));
        if (present_frame) {
            present_frame();
        }
    };

    SyncPortableCacheToLocal(ui_progress);

    if (const char* scan_env = std::getenv("BB_SCAN_SHADERBND");
        scan_env && *scan_env == '1') {
        ScanGameShaderArchives(ui_progress);
    }

    rasterizer.GetPipelineCache().WarmUp([&](u32 done, u32 total, u32 ok) {
        char buf[160]{};
        std::snprintf(buf, sizeof(buf),
                      "Warming up Bloodborne pipelines (%u / %u, ready: %u)...", done, total, ok);
        ui_progress(buf, done, total);
    });

    BbOverlay::SetLoading(false, 0, 0);
    if (present_frame) {
        present_frame();
    }
    CompactHostHeaps();
}

std::string RunFullVerifyAndWarmUp(Vulkan::Rasterizer& rasterizer,
                                   const std::function<void()>& present_frame) {
    auto ui_progress = [&](const std::string& label, u32 done, u32 total) {
        BbOverlay::SetLoadingLabel(label);
        BbOverlay::SetLoading(true, done, std::max(total, 1u));
        if (present_frame) {
            present_frame();
        }
    };

    const u32 imported = SyncPortableCacheToLocal(ui_progress);
    const ScanSummary scan = ScanGameShaderArchives(ui_progress);

    u32 warmed_ok = 0;
    u32 warmed_total = 0;
    rasterizer.GetPipelineCache().WarmUp([&](u32 done, u32 total, u32 ok) {
        warmed_total = total;
        warmed_ok = ok;
        char buf[160]{};
        std::snprintf(buf, sizeof(buf),
                      "Warming up Bloodborne pipelines (%u / %u, ready: %u)...", done, total, ok);
        ui_progress(buf, done, total);
    });

    const u32 exported = ExportLocalCacheToPortable();
    BbOverlay::SetLoading(false, 0, 0);
    if (present_frame) {
        present_frame();
    }
    CompactHostHeaps();

    char summary[320]{};
    std::snprintf(summary, sizeof(summary),
                  "Shaders verified: %u/%u archives (%u OrbShdr, %u unique) | "
                  "Warm-up: %u/%u PSOs | Portable sync: +%u in / +%u out",
                  scan.archives_ok, scan.archives_found, scan.total_orbshdr, scan.unique_hashes,
                  warmed_ok, warmed_total, imported, exported);
    return summary;
}

} // namespace BbPrecompile
