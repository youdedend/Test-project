// SPDX-License-Identifier: GPL-2.0-or-later
// Xbox UWP shader cache sync, Bloodborne .shaderbnd.dcx OrbShdr ahead-of-time DXIL precompiler,
// and pre-game warm-up UI.

#include "bb_precompile.h"
#include "bbport_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <miniz.h>
#include <sirit/sirit.h>

#include "common/assert.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/emit_spirv_quad_rect.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/regs_depth.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/amdgpu/regs_shader.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

#ifdef BB_UWP
#include <windows.h>
#include <fileapifromapp.h>
#include <malloc.h>
#include <winrt/Windows.System.h>
extern "C" void* runtime_low_stack_alloc(size_t size);
extern "C" void runtime_low_stack_free(void* ptr, size_t size);
#endif

namespace Shader {
size_t GetUwpSrtWalkerWatermark();
void RewindUwpSrtWalkerTo(size_t watermark);
} // namespace Shader

namespace BbPrecompile {

extern thread_local bool g_soft_assert_scope;

namespace {

constexpr u32 kSpirvVersion1_5 = 0x00010500u;
// Reserve 4 D3D12 VS input registers for SV_VertexID / SV_InstanceID / BaseVertex / BaseInstance.
constexpr u32 kMaxPrecompileVertexAttribs = 28u;
constexpr size_t kMaxLinkedPairs = 2048u;

std::atomic<bool> g_diag_active{false};
std::atomic<u32> g_diag_index{0};
std::atomic<u32> g_diag_total{0};
std::atomic<u32> g_diag_stage{0};
std::atomic<u64> g_diag_hash{0};
std::atomic<u64> g_diag_partner_hash{0};

struct ScopedActiveDiag {
    ScopedActiveDiag(u32 idx, u32 total, u32 stage, u64 hash, u64 partner = 0) noexcept {
        g_diag_index.store(idx, std::memory_order_relaxed);
        g_diag_total.store(total, std::memory_order_relaxed);
        g_diag_stage.store(stage, std::memory_order_relaxed);
        g_diag_hash.store(hash, std::memory_order_relaxed);
        g_diag_partner_hash.store(partner, std::memory_order_relaxed);
        g_diag_active.store(true, std::memory_order_release);
    }
    ~ScopedActiveDiag() noexcept {
        g_diag_active.store(false, std::memory_order_release);
    }
};

struct ScopedSoftAssert {
    bool prev;
    ScopedSoftAssert() : prev{g_soft_assert_scope} { g_soft_assert_scope = true; }
    ~ScopedSoftAssert() { g_soft_assert_scope = prev; }
};

void CompactHostHeaps() {
#ifdef BB_UWP
    _heapmin();
    if (const HANDLE heap = GetProcessHeap()) {
        HeapCompact(heap, 0);
    }
#endif
}

bool HasSafeMemoryHeadroom() {
#ifdef BB_UWP
    try {
        using winrt::Windows::System::MemoryManager;
        const u64 used = MemoryManager::AppMemoryUsage();
        const u64 limit = MemoryManager::AppMemoryUsageLimit();
        if (limit > 0 && used + (900ull << 20) >= limit) {
            return false;
        }
    } catch (...) {
    }
#endif
    return true;
}

u32 ReadBe32(const u8* p) {
    return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

u32 ReadLe32(const u8* p) {
    u32 v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

u16 ReadLe16(const u8* p) {
    u16 v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

u64 ReadLe64(const u8* p) {
    u64 v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

std::string ToLowerAscii(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

void EnsureDirTree(const std::filesystem::path& dir) {
    if (dir.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
#ifdef BB_UWP
    if (ec) {
        std::filesystem::path cur;
        for (const auto& part : dir) {
            cur /= part;
            if (!cur.empty()) {
                CreateDirectoryFromAppW(cur.wstring().c_str(), nullptr);
            }
        }
    }
#endif
}

bool HasValidVkCacheHeader(const u8* hdr, size_t len) {
    if (!hdr || len < 32) {
        return false;
    }
    return ReadLe32(hdr + 0) >= 32u && ReadLe32(hdr + 4) == 1u;
}

bool CopyFileViaIoFile(const std::filesystem::path& src, const std::filesystem::path& dst,
                       bool allow_uuid_replace = false) {
    Common::FS::IOFile in{src, Common::FS::FileAccessMode::Read};
    if (!in.IsOpen()) {
        return false;
    }
    const u64 size = in.GetSize();
    if (size == 0 || size > 512ull * 1024 * 1024) {
        return false;
    }
    const bool is_vkcache = dst.extension() == ".vkcache";
    std::array<u8, 32> src_hdr{};
    if (is_vkcache) {
        if (size < 32 || in.ReadRaw<u8>(src_hdr.data(), src_hdr.size()) != src_hdr.size() ||
            !HasValidVkCacheHeader(src_hdr.data(), src_hdr.size()) || !in.Seek(0)) {
            return false;
        }
    }
    {
        Common::FS::IOFile existing{dst, Common::FS::FileAccessMode::Read};
        if (existing.IsOpen()) {
            const u64 dst_size = existing.GetSize();
            if (is_vkcache && dst_size >= 32) {
                std::array<u8, 32> dst_hdr{};
                if (existing.ReadRaw<u8>(dst_hdr.data(), dst_hdr.size()) == dst_hdr.size() &&
                    HasValidVkCacheHeader(dst_hdr.data(), dst_hdr.size())) {
                    const bool same_uuid =
                        std::memcmp(src_hdr.data() + 8, dst_hdr.data() + 8, 24) == 0;
                    if (!same_uuid) {
                        if (!allow_uuid_replace) {
                            return false;
                        }
                    } else if (dst_size >= size) {
                        return false;
                    }
                }
            } else if (dst_size == size) {
                return false;
            }
        }
    }
    std::vector<u8> buf(static_cast<size_t>(size));
    if (in.ReadRaw<u8>(buf.data(), buf.size()) != buf.size()) {
        return false;
    }
    in.Close();

    EnsureDirTree(dst.parent_path());
    Common::FS::IOFile out{dst, Common::FS::FileAccessMode::Create};
    if (!out.IsOpen() || out.WriteRaw<u8>(buf.data(), buf.size()) != buf.size()) {
        out.Close();
        std::error_code ec;
        std::filesystem::remove(dst, ec);
#ifdef BB_UWP
        DeleteFileFromAppW(dst.wstring().c_str());
#endif
        return false;
    }
    return true;
}

u32 SyncDirectoryTree(const std::filesystem::path& src_root,
                      const std::filesystem::path& dst_root,
                      const ProgressFn& progress,
                      const char* label_prefix,
                      bool allow_uuid_replace = false) {
    if (src_root.empty() || dst_root.empty()) {
        return 0;
    }
    std::vector<std::filesystem::path> rel_files;
    std::error_code ec;
    if (std::filesystem::exists(src_root, ec)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator{src_root, ec}) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            const auto ext = entry.path().extension().string();
            if (ext == ".spv" || ext == ".meta" || ext == ".key" || ext == ".bin" ||
                ext == ".vkcache" || ext == ".tag") {
                const auto rel = std::filesystem::relative(entry.path(), src_root, ec);
                if (!ec && !rel.empty()) {
                    rel_files.push_back(rel);
                }
            }
        }
    }
    if (rel_files.empty()) {
        Common::FS::IOFile vkcache_probe{src_root / "driver.vkcache",
                                         Common::FS::FileAccessMode::Read};
        if (vkcache_probe.IsOpen()) {
            vkcache_probe.Close();
            rel_files.emplace_back("driver.vkcache");
        }
    }
    if (rel_files.empty()) {
        return 0;
    }
    u32 copied = 0;
    const u32 total = static_cast<u32>(rel_files.size());
    for (u32 i = 0; i < total; ++i) {
        if (CopyFileViaIoFile(src_root / rel_files[i], dst_root / rel_files[i],
                              allow_uuid_replace)) {
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

std::filesystem::path QuarantineFilePath() {
    const char* user_dir = std::getenv("BB_GPU_USER_DIR");
    if (!user_dir || !*user_dir) {
        return {};
    }
    return std::filesystem::path{user_dir}.parent_path() / "precompile_quarantine_v2.txt";
}

std::filesystem::path InflightFilePath() {
    const char* user_dir = std::getenv("BB_GPU_USER_DIR");
    if (!user_dir || !*user_dir) {
        return {};
    }
    return std::filesystem::path{user_dir}.parent_path() / "precompile_inflight.txt";
}

void AppendQuarantineHash(u64 hash) {
    if (hash == 0) {
        return;
    }
    const auto qpath = QuarantineFilePath();
    if (qpath.empty()) {
        return;
    }
#ifdef BB_UWP
    HANDLE qf = CreateFile2FromAppW(qpath.wstring().c_str(), FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS, nullptr);
    if (qf != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER zero{};
        SetFilePointerEx(qf, zero, nullptr, FILE_END);
        char line[32]{};
        const int n = std::snprintf(line, sizeof(line), "%016llx\n",
                                    static_cast<unsigned long long>(hash));
        DWORD written = 0;
        if (n > 0) {
            WriteFile(qf, line, static_cast<DWORD>(n), &written, nullptr);
        }
        CloseHandle(qf);
    }
#else
    if (FILE* f = std::fopen(qpath.string().c_str(), "ab")) {
        std::fprintf(f, "%016llx\n", static_cast<unsigned long long>(hash));
        std::fclose(f);
    }
#endif
}

void ConsumeInflightCrashBreadcrumb() {
    const auto ipath = InflightFilePath();
    if (ipath.empty()) {
        return;
    }
    {
        Common::FS::IOFile f{ipath, Common::FS::FileAccessMode::Read};
        if (f.IsOpen()) {
            const u64 sz = f.GetSize();
            if (sz > 0 && sz <= 256) {
                std::string text(static_cast<size_t>(sz), '\0');
                if (f.ReadRaw<char>(text.data(), text.size()) == text.size()) {
                    unsigned long long h = 0, partner = 0;
                    unsigned stage = 0, idx = 0;
                    if (std::sscanf(text.c_str(), "%llx %llx %u %u", &h, &partner, &stage, &idx) >=
                            1 &&
                        h != 0) {
                        std::printf(
                            "GPU: previous shader precompile died mid-compile on item %u "
                            "(stage=%u hash=%016llx partner=%016llx) — quarantining and resuming\n",
                            idx, stage, h, partner);
                        std::fflush(stdout);
                        if (stage == 3u && partner != 0) {
                            // Quarantine both members only if standalone didn't already clear them;
                            // encode pair quarantine key so standalone shaders stay unaffected.
                            const u64 pair_q = static_cast<u64>(h) ^
                                               (static_cast<u64>(partner) * 0x9E3779B97F4A7C15ull);
                            AppendQuarantineHash(pair_q);
                        } else {
                            AppendQuarantineHash(static_cast<u64>(h));
                        }
                    }
                }
            }
        }
    }
    std::error_code ec;
    std::filesystem::remove(ipath, ec);
#ifdef BB_UWP
    DeleteFileFromAppW(ipath.wstring().c_str());
#endif
}

class InflightBreadcrumb {
public:
    InflightBreadcrumb() {
        path_ = InflightFilePath();
        if (path_.empty()) {
            return;
        }
#ifdef BB_UWP
        handle_ = CreateFile2FromAppW(path_.wstring().c_str(), GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, CREATE_ALWAYS, nullptr);
#else
        file_ = std::fopen(path_.string().c_str(), "wb");
#endif
    }

    ~InflightBreadcrumb() { Clear(); }

    void Record(u32 idx, u32 stage, u64 hash, u64 partner) noexcept {
        char buf[64]{};
        const int n = std::snprintf(buf, sizeof(buf), "%016llx %016llx %02u %08u\n",
                                    static_cast<unsigned long long>(hash),
                                    static_cast<unsigned long long>(partner), stage, idx);
        if (n <= 0) {
            return;
        }
#ifdef BB_UWP
        if (handle_ != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER zero{};
            SetFilePointerEx(handle_, zero, nullptr, FILE_BEGIN);
            DWORD written = 0;
            WriteFile(handle_, buf, static_cast<DWORD>(n), &written, nullptr);
        }
#else
        if (file_) {
            std::fseek(file_, 0, SEEK_SET);
            std::fwrite(buf, 1, static_cast<size_t>(n), file_);
            std::fflush(file_);
        }
#endif
    }

    void Clear() noexcept {
#ifdef BB_UWP
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        if (!path_.empty()) {
            DeleteFileFromAppW(path_.wstring().c_str());
            path_.clear();
        }
#else
        if (file_) {
            std::fclose(file_);
            file_ = nullptr;
        }
        if (!path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
            path_.clear();
        }
#endif
    }

private:
    std::filesystem::path path_;
#ifdef BB_UWP
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    FILE* file_ = nullptr;
#endif
};

std::unordered_set<u64> LoadQuarantineHashes() {
    std::unordered_set<u64> out;
    const auto path = QuarantineFilePath();
    if (path.empty()) {
        return out;
    }
    Common::FS::IOFile f{path, Common::FS::FileAccessMode::Read};
    if (!f.IsOpen()) {
        return out;
    }
    const u64 sz = f.GetSize();
    if (sz == 0 || sz > 256 * 1024) {
        return out;
    }
    std::string text(static_cast<size_t>(sz), '\0');
    if (f.ReadRaw<char>(text.data(), text.size()) != text.size()) {
        return out;
    }
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) {
            nl = text.size();
        }
        std::string line = text.substr(pos, nl - pos);
        char* endp = nullptr;
        const unsigned long long h = std::strtoull(line.c_str(), &endp, 16);
        if (endp != line.c_str() && h != 0) {
            out.insert(static_cast<u64>(h));
        }
        pos = nl + 1;
    }
    return out;
}

bool DecompressDcxSpan(std::span<const u8> raw, std::vector<u8>& out_data) {
    out_data.clear();
    if (raw.size() < 16) {
        return false;
    }
    if (std::memcmp(raw.data(), "DCX\0", 4) != 0) {
        out_data.assign(raw.begin(), raw.end());
        return true;
    }
    if (raw.size() < 0x4C) {
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
    if (uncomp_size == 0 || uncomp_size > 128u * 1024 * 1024 || comp_size == 0) {
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

bool LoadShaderContainer(const std::filesystem::path& path, std::vector<u8>& out_data) {
    out_data.clear();
    Common::FS::IOFile file{path, Common::FS::FileAccessMode::Read};
    if (!file.IsOpen()) {
        return false;
    }
    const u64 file_size = file.GetSize();
    if (file_size < 16 || file_size > 96ull * 1024 * 1024) {
        return false;
    }
    std::vector<u8> raw(static_cast<size_t>(file_size));
    if (file.ReadRaw<u8>(raw.data(), raw.size()) != raw.size()) {
        return false;
    }
    file.Close();
    return DecompressDcxSpan(raw, out_data);
}

bool IsShaderArchiveFilename(const std::string& lower_name) {
    return lower_name.ends_with(".shaderbnd.dcx") || lower_name.ends_with(".shaderbnd") ||
           lower_name.ends_with(".gxsh") || lower_name.ends_with(".gxsh.dcx");
}

void CollectShaderFilesRecursive(const std::filesystem::path& root,
                                 std::unordered_map<std::string, std::filesystem::path>& by_key,
                                 u32 max_depth = 5) {
    if (root.empty()) {
        return;
    }
#ifdef BB_UWP
    auto walk = [&](auto&& self, const std::filesystem::path& dir, u32 depth) -> void {
        if (depth > max_depth) {
            return;
        }
        WIN32_FIND_DATAW ffd{};
        const std::wstring pattern = (dir / L"*").wstring();
        const HANDLE h = FindFirstFileExFromAppW(pattern.c_str(), FindExInfoBasic, &ffd,
                                                 FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) {
            return;
        }
        do {
            if (std::wcscmp(ffd.cFileName, L".") == 0 || std::wcscmp(ffd.cFileName, L"..") == 0) {
                continue;
            }
            const auto child = dir / ffd.cFileName;
            if ((ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                self(self, child, depth + 1);
            } else {
                const std::string lower = ToLowerAscii(child.filename().string());
                if (IsShaderArchiveFilename(lower)) {
                    by_key[lower] = child;
                }
            }
        } while (FindNextFileW(h, &ffd));
        FindClose(h);
    };
    walk(walk, root, 0);
#else
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        return;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator{root, ec}) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::string lower = ToLowerAscii(entry.path().filename().string());
        if (IsShaderArchiveFilename(lower)) {
            by_key[lower] = entry.path();
        }
    }
#endif
}

std::vector<std::filesystem::path> FindShaderBndArchives() {
    std::unordered_map<std::string, std::filesystem::path> by_key;

    if (const char* dump = std::getenv("BB_UWP_DUMP"); dump && *dump) {
        const std::filesystem::path dump_root{dump};
        CollectShaderFilesRecursive(dump_root / "dvdroot_ps4", by_key, 4);

        if (by_key.empty()) {
            const auto shader_dir = dump_root / "dvdroot_ps4" / "shader";
            static constexpr std::array<const char*, 14> kKnownArchives = {
                "allmaterialbnd.shaderbnd.dcx",
                "material.shaderbnd.dcx",
                "sfxshaderbnd.shaderbnd.dcx",
                "sfxshader.shaderbnd.dcx",
                "gxshader.shaderbnd.dcx",
                "gxgui.shaderbnd.dcx",
                "gxrenderershader.shaderbnd.dcx",
                "menushader.shaderbnd.dcx",
                "debugshader.shaderbnd.dcx",
                "filtershader.shaderbnd.dcx",
                "MenuShader.shaderbnd.dcx",
                "DebugShader.shaderbnd.dcx",
                "FilterShader.shaderbnd.dcx",
                "GXShader.shaderbnd.dcx",
            };
            for (const char* name : kKnownArchives) {
                const std::string lower = ToLowerAscii(name);
                if (by_key.contains(lower)) {
                    continue;
                }
                const auto candidate = shader_dir / name;
                Common::FS::IOFile probe{candidate, Common::FS::FileAccessMode::Read};
                if (probe.IsOpen()) {
                    probe.Close();
                    by_key[lower] = candidate;
                }
            }
        }
    }

    // Layer DLC and loose-file mods on top (matching runtime_file.c precedence).
    if (const char* dlc_env = std::getenv("BB_DLC_DIR"); dlc_env && *dlc_env) {
        CollectShaderFilesRecursive(std::filesystem::path{dlc_env}, by_key, 5);
    }
    if (const char* mods_env = std::getenv("BB_MODS_DIR"); mods_env && *mods_env) {
        CollectShaderFilesRecursive(std::filesystem::path{mods_env}, by_key, 6);
    }
    if (const char* user_dir = std::getenv("BB_GPU_USER_DIR"); user_dir && *user_dir) {
        const auto local_state = std::filesystem::path{user_dir}.parent_path();
        CollectShaderFilesRecursive(local_state / "dlc", by_key, 5);
        CollectShaderFilesRecursive(local_state / "mods", by_key, 6);
    }

    std::vector<std::filesystem::path> archives;
    archives.reserve(by_key.size());
    for (auto& [_, p] : by_key) {
        archives.push_back(std::move(p));
    }
    std::ranges::sort(archives);
    return archives;
}

std::filesystem::path FindPreparedBootBin() {
    auto try_dir = [](const std::filesystem::path& dir) -> std::filesystem::path {
        for (const char* name : {"boot-linked.bin", "eboot.elf", "boot.bin"}) {
            const auto p = dir / name;
            Common::FS::IOFile probe{p, Common::FS::FileAccessMode::Read};
            if (probe.IsOpen()) {
                return p;
            }
        }
        return {};
    };
    if (const char* prep = std::getenv("BB_UWP_PREPARED"); prep && *prep) {
        if (auto p = try_dir(std::filesystem::path{prep}); !p.empty()) {
            return p;
        }
    }
    if (const char* user_dir = std::getenv("BB_GPU_USER_DIR"); user_dir && *user_dir) {
        if (auto p = try_dir(std::filesystem::path{user_dir}.parent_path() / "Prepared");
            !p.empty()) {
            return p;
        }
    }
    return {};
}

std::string NormalizeShaderStem(std::string name) {
    for (char& c : name) {
        if (c == '\\') {
            c = '/';
        }
    }
    if (const size_t slash = name.rfind('/'); slash != std::string::npos) {
        name.erase(0, slash + 1);
    }
    name = ToLowerAscii(std::move(name));
    if (const size_t dot = name.find('.'); dot != std::string::npos) {
        name.erase(dot);
    }
    for (const char* suf : {"_vs", "_ps", "_vv", "_p", "_v", "_f"}) {
        const size_t len = std::strlen(suf);
        if (name.size() > len && name.ends_with(suf)) {
            name.erase(name.size() - len);
            break;
        }
    }
    return name;
}

std::string ReadBnd4EntryName(std::span<const u8> bnd, size_t entry_off) {
    if (entry_off + 0x24 > bnd.size() || bnd.size() < 0x40) {
        return {};
    }
    const u32 name_off = ReadLe32(bnd.data() + entry_off + 0x20);
    if (name_off >= bnd.size()) {
        return {};
    }
    std::string out;
    if (bnd[0x30] != 0) {
        for (size_t p = name_off; p + 1 < bnd.size() && out.size() < 256; p += 2) {
            const u16 ch = ReadLe16(bnd.data() + p);
            if (ch == 0) {
                break;
            }
            out.push_back(ch < 128 ? static_cast<char>(ch) : '_');
        }
    } else {
        for (size_t p = name_off; p < bnd.size() && out.size() < 256; ++p) {
            const u8 ch = bnd[p];
            if (ch == 0) {
                break;
            }
            out.push_back(ch < 128 ? static_cast<char>(ch) : '_');
        }
    }
    return NormalizeShaderStem(std::move(out));
}

template <typename Fn>
void ForEachContainerSlice(std::span<const u8> payload, Fn&& fn) {
    if (payload.size() >= 0x40 && std::memcmp(payload.data(), "BND4", 4) == 0) {
        const u32 count = ReadLe32(payload.data() + 0x0C);
        const u64 entry_size = ReadLe64(payload.data() + 0x20);
        if (entry_size >= 0x20 && entry_size <= 0x40 &&
            0x40ull + u64(count) * entry_size <= payload.size()) {
            std::vector<u8> nested_buf;
            for (u32 i = 0; i < count; ++i) {
                const size_t entry = 0x40 + size_t(i) * static_cast<size_t>(entry_size);
                const u64 len = ReadLe64(payload.data() + entry + 0x08);
                const u32 off = ReadLe32(payload.data() + entry + 0x18);
                if (len == 0 || u64(off) + len > payload.size()) {
                    continue;
                }
                const std::string stem =
                    entry_size >= 0x24 ? ReadBnd4EntryName(payload, entry) : std::string{};
                std::span<const u8> slice{payload.data() + off, static_cast<size_t>(len)};
                if (slice.size() >= 0x4C && std::memcmp(slice.data(), "DCX\0", 4) == 0) {
                    if (DecompressDcxSpan(slice, nested_buf)) {
                        fn(std::span<const u8>{nested_buf}, stem);
                    }
                } else {
                    fn(slice, stem);
                }
            }
            return;
        }
    }
    fn(payload, std::string{});
}

struct OrbProgram {
    size_t start = 0;
    size_t footer = 0;
    u64 hash = 0;
    u32 bytes = 0;
    u32 type = 0; // 0=PS, 1=VS, 2=GS, 3=CS, 4=HS, 5=DS, 6=ES, 7=LS
    u32 usages = 0;
    bool is_srt = false;
};

std::optional<OrbProgram> ParseOrbProgramAt(std::span<const u8> data, size_t at) {
    if (at + 8 > data.size() || ReadLe32(data.data() + at) != 0xbeeb03ffu) {
        return std::nullopt;
    }
    const u64 delta = (u64(ReadLe32(data.data() + at + 4)) + 1ull) * 8ull;
    if (delta < 8 || delta > 262144 || at + delta + 28 > data.size()) {
        return std::nullopt;
    }
    const u8* footer = data.data() + at + delta;
    if (std::memcmp(footer, "OrbShdr", 7) != 0) {
        return std::nullopt;
    }
    const u32 flags_len = ReadLe32(footer + 8);
    const u32 length = flags_len >> 8;
    if (length == 0 || (length % 4) != 0 || length > delta) {
        return std::nullopt;
    }
    return OrbProgram{
        .start = at,
        .footer = at + static_cast<size_t>(delta),
        .hash = ReadLe64(footer + 16),
        .bytes = length,
        .type = (flags_len >> 2) & 0xFu,
        .usages = footer[13],
        .is_srt = (footer[14] & 1u) != 0,
    };
}

enum class StageKind : u8 {
    Compute = 0,
    Fragment = 1,
    Vertex = 2,
};

struct ExtractedShader {
    StageKind kind{};
    u64 hash = 0;
    bool is_srt = false;
    std::array<u32, 13> regs{};
    std::vector<u32> usages;
    std::vector<u32> semantics; // VS: packed input semantics; PS: raw u16 input semantics
    std::vector<std::pair<u8, u8>> vs_exports; // VS: (semantic_id -> export_param_index)
    std::vector<u32> code;
};

bool MapVsOutputsSafe(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl,
                      u32& out_count) {
    out_count = 0;
    if (ctl.use_vtx_edge_flag || ctl.use_vtx_kill_flag || ctl.use_vtx_gs_cut_flag) {
        return false;
    }
    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[out_count++];
        misc_vec[0] = ctl.use_vtx_point_size ? Shader::Output::PointSize : Shader::Output::None;
        misc_vec[1] = Shader::Output::None;
        misc_vec[2] = ctl.use_vtx_render_target_idx ? Shader::Output::RenderTargetIndex
                                                    : Shader::Output::None;
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Shader::Output::ViewportIndex
                                               : Shader::Output::None;
    }
    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[out_count++];
        ccdist0[0] = ctl.IsClipDistEnabled(0)
                         ? Shader::Output::ClipDist0
                         : (ctl.IsCullDistEnabled(0) ? Shader::Output::CullDist0
                                                     : Shader::Output::None);
        ccdist0[1] = ctl.IsClipDistEnabled(1)
                         ? Shader::Output::ClipDist1
                         : (ctl.IsCullDistEnabled(1) ? Shader::Output::CullDist1
                                                     : Shader::Output::None);
        ccdist0[2] = ctl.IsClipDistEnabled(2)
                         ? Shader::Output::ClipDist2
                         : (ctl.IsCullDistEnabled(2) ? Shader::Output::CullDist2
                                                     : Shader::Output::None);
        ccdist0[3] = ctl.IsClipDistEnabled(3)
                         ? Shader::Output::ClipDist3
                         : (ctl.IsCullDistEnabled(3) ? Shader::Output::CullDist3
                                                     : Shader::Output::None);
    }
    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[out_count++];
        ccdist1[0] = ctl.IsClipDistEnabled(4)
                         ? Shader::Output::ClipDist4
                         : (ctl.IsCullDistEnabled(4) ? Shader::Output::CullDist4
                                                     : Shader::Output::None);
        ccdist1[1] = ctl.IsClipDistEnabled(5)
                         ? Shader::Output::ClipDist5
                         : (ctl.IsCullDistEnabled(5) ? Shader::Output::CullDist5
                                                     : Shader::Output::None);
        ccdist1[2] = ctl.IsClipDistEnabled(6)
                         ? Shader::Output::ClipDist6
                         : (ctl.IsCullDistEnabled(6) ? Shader::Output::CullDist6
                                                     : Shader::Output::None);
        ccdist1[3] = ctl.IsClipDistEnabled(7)
                         ? Shader::Output::ClipDist7
                         : (ctl.IsCullDistEnabled(7) ? Shader::Output::CullDist7
                                                     : Shader::Output::None);
    }
    return out_count <= 3u;
}

bool TryExtractCs(std::span<const u8> data, const OrbProgram& p, ExtractedShader& out) {
    static constexpr std::array<size_t, 8> kCsBaseSizes = {36, 40, 44, 48, 32, 52, 56, 60};
    for (const size_t base : kCsBaseSizes) {
        const size_t size = base + size_t(p.usages) * 4;
        if (size > p.start) {
            continue;
        }
        const u8* h = data.data() + p.start - size;
        if (ReadLe32(h) != size + 8 || ReadLe32(h + 4) != 0xffffffffu || ReadLe32(h + 8) != 0u) {
            continue;
        }
        if (((ReadLe32(h + 16) >> 1) & 31u) > 16u) {
            continue;
        }
        const u32 ntx = ReadLe32(h + 20) & 0xffffu;
        const u32 nty = ReadLe32(h + 24) & 0xffffu;
        const u32 ntz = ReadLe32(h + 28) & 0xffffu;
        if (ntx == 0 || nty == 0 || ntz == 0 || u64(ntx) * nty * ntz > 1024ull) {
            continue;
        }
        out = {};
        out.kind = StageKind::Compute;
        out.hash = p.hash;
        out.is_srt = p.is_srt;
        std::memcpy(out.regs.data(), h, std::min<size_t>(32, base));
        out.usages.resize(p.usages);
        if (p.usages > 0) {
            std::memcpy(out.usages.data(), h + base, size_t(p.usages) * 4);
        }
        out.code.resize(p.bytes / 4);
        std::memcpy(out.code.data(), data.data() + p.start, p.bytes);
        return true;
    }
    return false;
}

bool TryExtractPs(std::span<const u8> data, const OrbProgram& p, ExtractedShader& out) {
    for (u32 count = 0; count <= 32; ++count) {
        const size_t size = 52 + size_t(p.usages) * 4 + ((size_t(count) * 2 + 3) & ~size_t(3));
        if (size > p.start) {
            continue;
        }
        const u8* h = data.data() + p.start - size;
        if (ReadLe32(h) != size + 8 || ReadLe32(h + 4) != 0xffffffffu ||
            ReadLe32(h + 48) != count || (ReadLe32(h + 32) & 63u) != count ||
            ((ReadLe32(h + 12) >> 1) & 31u) > 16u) {
            continue;
        }
        out = {};
        out.kind = StageKind::Fragment;
        out.hash = p.hash;
        out.is_srt = p.is_srt;
        std::memcpy(out.regs.data(), h, 52);
        out.usages.resize(p.usages);
        if (p.usages > 0) {
            std::memcpy(out.usages.data(), h + 52, size_t(p.usages) * 4);
        }
        out.semantics.resize(count);
        const u8* sem_table = h + 52 + size_t(p.usages) * 4;
        for (u32 i = 0; i < count; ++i) {
            out.semantics[i] = ReadLe16(sem_table + i * 2);
        }
        out.code.resize(p.bytes / 4);
        std::memcpy(out.code.data(), data.data() + p.start, p.bytes);
        return true;
    }
    return false;
}

bool TryExtractVs(std::span<const u8> data, const OrbProgram& p, ExtractedShader& out) {
    for (size_t size = 32; size <= std::min<size_t>(p.start, 1280); size += 4) {
        const u8* h = data.data() + p.start - size;
        if (ReadLe32(h) != size + 8 || ReadLe32(h + 4) != 0xffffffffu) {
            continue;
        }
        const u32 counts = ReadLe32(h + 28);
        const u32 inputs = counts & 255u;
        const u32 outputs = (counts >> 8) & 255u;
        if ((counts >> 16) != 0 || inputs > kMaxPrecompileVertexAttribs || outputs > 32) {
            continue;
        }
        const size_t expected = 32 + size_t(p.usages) * 4 + size_t(inputs) * 4 +
                                ((size_t(outputs) * 2 + 3) & ~size_t(3));
        if (size != expected || ((ReadLe32(h + 12) >> 1) & 31u) > 16u) {
            continue;
        }
        std::array<Shader::OutputMap, 3> dummy_outputs{};
        u32 mapped_out = 0;
        if (!MapVsOutputsSafe(dummy_outputs, std::bit_cast<AmdGpu::VsOutputControl>(ReadLe32(h + 24)),
                              mapped_out)) {
            return false;
        }

        const u8* exp_table = h + 32 + size_t(p.usages) * 4 + size_t(inputs) * 4;
        std::vector<std::pair<u8, u8>> exports;
        exports.reserve(outputs);
        for (u32 i = 0; i < outputs; ++i) {
            const u16 val = ReadLe16(exp_table + i * 2);
            const u8 sem = static_cast<u8>(val & 0xFFu);
            const u8 idx = static_cast<u8>((val >> 8) & 0xFFu);
            if (idx >= 32u) {
                return false;
            }
            exports.emplace_back(sem, idx);
        }

        out = {};
        out.kind = StageKind::Vertex;
        out.hash = p.hash;
        out.is_srt = p.is_srt;
        std::memcpy(out.regs.data(), h, 32);
        out.usages.resize(p.usages);
        if (p.usages > 0) {
            std::memcpy(out.usages.data(), h + 32, size_t(p.usages) * 4);
        }
        out.semantics.resize(inputs);
        if (inputs > 0) {
            std::memcpy(out.semantics.data(), h + 32 + size_t(p.usages) * 4, size_t(inputs) * 4);
        }
        out.vs_exports = std::move(exports);
        out.code.resize(p.bytes / 4);
        std::memcpy(out.code.data(), data.data() + p.start, p.bytes);
        return true;
    }
    return false;
}

// 1 MiB < 1 TiB synthetic GNM descriptor arena so ResourceDiscoverPass & ParseFetchShader read
// valid 48-bit PS4 pointers without touching live guest memory.
class SyntheticGnmArena {
public:
    static constexpr size_t kArenaBytes = 1024 * 1024;
    static constexpr size_t kExtendedOff = 0x00000;   // 128 KiB
    static constexpr size_t kSrtLeafOff = 0x20000;    // 128 KiB
    static constexpr size_t kResourceOff = 0x40000;   // 256 KiB (T# images)
    static constexpr size_t kConstBufOff = 0x80000;   // 256 KiB (V# buffers)
    static constexpr size_t kSamplerOff = 0xC0000;    // 128 KiB (S# samplers)
    static constexpr size_t kVertexOff = 0xE0000;     // 64 KiB  (vertex V# table)
    static constexpr size_t kFetchCodeOff = 0xF0000;  // 64 KiB  (GCN fetch shader ISA)

    SyntheticGnmArena() {
#ifdef BB_UWP
        base_ = static_cast<u8*>(runtime_low_stack_alloc(kArenaBytes));
#else
        base_ = static_cast<u8*>(std::calloc(1, kArenaBytes));
#endif
        if (!base_) {
            return;
        }
        std::memset(base_, 0, kArenaBytes);

        AmdGpu::Image img = AmdGpu::Image::Null(false);
        img.base_address = 1;
        img.width = 63;
        img.height = 63;
        img.pitch = 63;

        AmdGpu::Buffer cbuf = AmdGpu::Buffer::Placeholder(65536);
        cbuf.data_format = u32(AmdGpu::DataFormat::Format32_32_32_32);
        cbuf.num_format = u32(AmdGpu::NumberFormat::Float);
        cbuf.stride = 16;
        cbuf.dst_sel_x = u32(AmdGpu::CompSwizzle::Red);
        cbuf.dst_sel_y = u32(AmdGpu::CompSwizzle::Green);
        cbuf.dst_sel_z = u32(AmdGpu::CompSwizzle::Blue);
        cbuf.dst_sel_w = u32(AmdGpu::CompSwizzle::Alpha);

        AmdGpu::Sampler smp{};

        for (size_t off = 0; off + sizeof(img) <= 0x40000; off += sizeof(img)) {
            std::memcpy(base_ + kResourceOff + off, &img, sizeof(img));
        }
        for (size_t off = 0; off + sizeof(cbuf) <= 0x40000; off += sizeof(cbuf)) {
            std::memcpy(base_ + kConstBufOff + off, &cbuf, sizeof(cbuf));
        }
        for (size_t off = 0; off + sizeof(cbuf) <= 0x20000; off += sizeof(cbuf)) {
            std::memcpy(base_ + kSrtLeafOff + off, &cbuf, sizeof(cbuf));
        }
        for (size_t off = 0; off + sizeof(smp) <= 0x20000; off += sizeof(smp)) {
            std::memcpy(base_ + kSamplerOff + off, &smp, sizeof(smp));
        }
        for (size_t off = 0; off + sizeof(cbuf) <= 0x10000; off += sizeof(cbuf)) {
            std::memcpy(base_ + kVertexOff + off, &cbuf, sizeof(cbuf));
        }
    }

    ~SyntheticGnmArena() {
        if (base_) {
#ifdef BB_UWP
            runtime_low_stack_free(base_, kArenaBytes);
#else
            std::free(base_);
#endif
        }
    }

    bool Valid() const noexcept {
        return base_ != nullptr && reinterpret_cast<uintptr_t>(base_) >= 0x10000u;
    }

    bool Populate(const ExtractedShader& sh, std::array<u32, 16>& user_regs) {
        if (!Valid()) {
            return false;
        }
        user_regs.fill(0);

        const u64 leaf_ptr = PtrAt(kSrtLeafOff);
        for (size_t off = 0; off + sizeof(u64) <= 0x20000; off += sizeof(u64)) {
            std::memcpy(base_ + kExtendedOff + off, &leaf_ptr, sizeof(u64));
        }
        for (u32 r = 0; r + 1 < 16; r += 2) {
            std::memcpy(&user_regs[r], &leaf_ptr, sizeof(u64));
        }

        AmdGpu::Image img = AmdGpu::Image::Null(false);
        img.base_address = 1;
        img.width = 63;
        img.height = 63;
        img.pitch = 63;

        AmdGpu::Buffer buf = AmdGpu::Buffer::Placeholder(65536);
        buf.data_format = u32(AmdGpu::DataFormat::Format32_32_32_32);
        buf.num_format = u32(AmdGpu::NumberFormat::Float);
        buf.stride = 16;
        buf.dst_sel_x = u32(AmdGpu::CompSwizzle::Red);
        buf.dst_sel_y = u32(AmdGpu::CompSwizzle::Green);
        buf.dst_sel_z = u32(AmdGpu::CompSwizzle::Blue);
        buf.dst_sel_w = u32(AmdGpu::CompSwizzle::Alpha);

        AmdGpu::Sampler smp{};

        auto put = [&](u32 reg, const void* val, size_t nbytes) -> bool {
            if (reg < 16) {
                if (size_t(reg) * 4 + nbytes > sizeof(user_regs)) {
                    return false;
                }
                std::memcpy(reinterpret_cast<u8*>(user_regs.data()) + size_t(reg) * 4, val, nbytes);
                return true;
            }
            const size_t ext_byte = size_t(reg - 16) * 4;
            if (ext_byte + nbytes > 0x20000) {
                return false;
            }
            std::memcpy(base_ + kExtendedOff + ext_byte, val, nbytes);
            return true;
        };

        std::optional<u32> vtx_table_reg;
        std::optional<u32> fetch_subptr_reg;

        for (const u32 usage : sh.usages) {
            const u32 type = usage & 0xFFu;
            const u32 reg = (usage >> 16) & 0xFFu;
            const u32 flags = (usage >> 24) & 0xFFu;
            switch (type) {
            case 0x00: // ImmResource (V# or T#)
            case 0x04: // ImmRwResource
                if (flags & 2u) {
                    if (!put(reg, &img, sizeof(img))) return false;
                } else {
                    if (!put(reg, &buf, sizeof(buf))) return false;
                }
                break;
            case 0x01: // ImmSampler
                if (!put(reg, &smp, sizeof(smp))) return false;
                break;
            case 0x02: // ImmConstBuffer
            case 0x03: // ImmVertexBuffer
                if (!put(reg, &buf, sizeof(buf))) return false;
                break;
            case 0x05: // ImmAluFloatConst
            case 0x06: // ImmAluBool32Const
            case 0x07: // ImmGdsCounterRange
            case 0x08: // ImmGdsMemoryRange
            case 0x09: // ImmGwsBase
                break;
            case 0x0D: // ImmShaderResourceTable / SRT pointer
            case 0x1D: // PtrIndirectResourceTable
                if (!put(reg, &leaf_ptr, sizeof(u64))) return false;
                break;
            case 0x12: // SubPtrFetchShader
                fetch_subptr_reg = reg;
                break;
            case 0x13: // PtrResourceTable
            case 0x14: // PtrInternalResourceTable
            case 0x19: // PtrRwResourceTable
            {
                const u64 p = PtrAt(kResourceOff);
                if (!put(reg, &p, sizeof(u64))) return false;
                break;
            }
            case 0x15: // PtrSamplerTable
            {
                const u64 p = PtrAt(kSamplerOff);
                if (!put(reg, &p, sizeof(u64))) return false;
                break;
            }
            case 0x16: // PtrConstBufferTable
            case 0x18: // PtrSoBufferTable
            {
                const u64 p = PtrAt(kConstBufOff);
                if (!put(reg, &p, sizeof(u64))) return false;
                break;
            }
            case 0x17: // PtrVertexBufferTable
            {
                vtx_table_reg = reg;
                const u64 p = PtrAt(kVertexOff);
                if (!put(reg, &p, sizeof(u64))) return false;
                break;
            }
            case 0x1B: // PtrExtendedUserData
            {
                const u64 p = PtrAt(kExtendedOff);
                if (!put(reg, &p, sizeof(u64))) return false;
                break;
            }
            default:
                return false;
            }
        }

        if (sh.kind == StageKind::Vertex) {
            if (fetch_subptr_reg) {
                const u32 vtable = vtx_table_reg.value_or(2u);
                if (!vtx_table_reg && vtable < 15) {
                    const u64 p = PtrAt(kVertexOff);
                    put(vtable, &p, sizeof(u64));
                }
                if (vtable >= 128 || (vtable & 1u) != 0) {
                    return false;
                }
                if (!BuildFetchShader(sh.semantics, vtable)) {
                    return false;
                }
                const u64 fetch_ptr = PtrAt(kFetchCodeOff + fetch_gen_offset_);
                if (!put(*fetch_subptr_reg, &fetch_ptr, sizeof(u64))) {
                    return false;
                }
            } else if (!sh.semantics.empty()) {
                return false;
            }
        }
        return true;
    }

private:
    u64 PtrAt(size_t offset) const noexcept {
        return static_cast<u64>(reinterpret_cast<uintptr_t>(base_ + offset));
    }

    bool BuildFetchShader(std::span<const u32> semantics, u32 vtable_sgpr) {
        if (semantics.size() > kMaxPrecompileVertexAttribs) {
            return false;
        }
        fetch_cursor_ = (fetch_cursor_ + 512) % 0xF000u;
        fetch_gen_offset_ = fetch_cursor_;
        u32* out = reinterpret_cast<u32*>(base_ + kFetchCodeOff + fetch_gen_offset_);
        size_t w = 0;
        for (size_t i = 0; i < semantics.size(); ++i) {
            const u32 packed = semantics[i];
            const u32 vgpr = (packed >> 8) & 0xFFu;
            const u32 comps = std::clamp<u32>((packed >> 16) & 0xFFu, 1u, 4u);
            if (vgpr > 252) {
                return false;
            }
            // s_load_dwordx4 s[4:7], s[vtable:vtable+1], (i * 4)
            out[w++] = 0xC0820100u | (((vtable_sgpr / 2u) & 0x3Fu) << 9) | (u32(i * 4) & 0xFFu);
            // buffer_load_format_{x..xyzw} v[vgpr], v0, s[4:7], 0 idxen
            out[w++] = 0xE0002000u | (((comps - 1u) & 3u) << 18);
            out[w++] = 0x80010000u | ((vgpr & 0xFFu) << 8);
        }
        // s_setpc_b64 s[0:1]
        out[w++] = 0xBE802000u;
        return true;
    }

    u8* base_ = nullptr;
    size_t fetch_cursor_ = 0;
    size_t fetch_gen_offset_ = 0;
};

std::vector<u32> EmitPassthroughVertexSpirv(u32 num_outputs) {
    Sirit::Module m{kSpirvVersion1_5};
    m.AddCapability(spv::Capability::Shader);
    m.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);

    const Sirit::Id void_ty = m.TypeVoid();
    const Sirit::Id f32_ty = m.TypeFloat(32);
    const Sirit::Id vec4_ty = m.TypeVector(f32_ty, 4);
    const Sirit::Id ptr_out_vec4 = m.TypePointer(spv::StorageClass::Output, vec4_ty);
    const Sirit::Id fn_ty = m.TypeFunction(void_ty);

    const Sirit::Id f0 = m.Constant(f32_ty, 0.0f);
    const Sirit::Id f1 = m.Constant(f32_ty, 1.0f);
    const Sirit::Id zero4 = m.ConstantComposite(vec4_ty, f0, f0, f0, f0);
    const Sirit::Id pos4 = m.ConstantComposite(vec4_ty, f0, f0, f0, f1);

    std::vector<Sirit::Id> iface;
    const Sirit::Id gl_pos = m.AddGlobalVariable(ptr_out_vec4, spv::StorageClass::Output);
    m.Decorate(gl_pos, spv::Decoration::BuiltIn, spv::BuiltIn::Position);
    iface.push_back(gl_pos);

    std::vector<Sirit::Id> outs;
    outs.reserve(num_outputs);
    for (u32 i = 0; i < num_outputs; ++i) {
        const Sirit::Id v = m.AddGlobalVariable(ptr_out_vec4, spv::StorageClass::Output);
        m.Decorate(v, spv::Decoration::Location, i);
        iface.push_back(v);
        outs.push_back(v);
    }

    const Sirit::Id main_fn = m.OpFunction(void_ty, spv::FunctionControlMask::MaskNone, fn_ty);
    m.AddEntryPoint(spv::ExecutionModel::Vertex, main_fn, "main", std::span<const Sirit::Id>{iface});
    m.AddLabel(m.OpLabel());
    m.OpStore(gl_pos, pos4);
    for (const Sirit::Id v : outs) {
        m.OpStore(v, zero4);
    }
    m.OpReturn();
    m.OpFunctionEnd();
    return m.Assemble();
}

std::vector<u32> EmitPassthroughFragmentSpirv() {
    Sirit::Module m{kSpirvVersion1_5};
    m.AddCapability(spv::Capability::Shader);
    m.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);

    const Sirit::Id void_ty = m.TypeVoid();
    const Sirit::Id f32_ty = m.TypeFloat(32);
    const Sirit::Id vec4_ty = m.TypeVector(f32_ty, 4);
    const Sirit::Id ptr_out_vec4 = m.TypePointer(spv::StorageClass::Output, vec4_ty);
    const Sirit::Id fn_ty = m.TypeFunction(void_ty);

    const Sirit::Id f0 = m.Constant(f32_ty, 0.0f);
    const Sirit::Id zero4 = m.ConstantComposite(vec4_ty, f0, f0, f0, f0);

    const Sirit::Id out0 = m.AddGlobalVariable(ptr_out_vec4, spv::StorageClass::Output);
    m.Decorate(out0, spv::Decoration::Location, 0u);
    const std::array<Sirit::Id, 1> iface{out0};

    const Sirit::Id main_fn = m.OpFunction(void_ty, spv::FunctionControlMask::MaskNone, fn_ty);
    m.AddEntryPoint(spv::ExecutionModel::Fragment, main_fn, "main",
                    std::span<const Sirit::Id>{iface});
    m.AddExecutionMode(main_fn, spv::ExecutionMode::OriginUpperLeft);
    m.AddLabel(m.OpLabel());
    m.OpStore(out0, zero4);
    m.OpReturn();
    m.OpFunctionEnd();
    return m.Assemble();
}

struct PrecompileContext {
    Vulkan::PipelineCache& cache;
    SyntheticGnmArena& arena;
    std::array<std::vector<u32>, 33> passthrough_vs_spv;
    std::vector<u32> passthrough_fs_spv;
    Shader::Info dummy_vs_info{};
    Shader::Info dummy_fs_info{};
    Shader::RuntimeInfo dummy_vs_rt{};
    Shader::RuntimeInfo dummy_fs_rt{};

    PrecompileContext(Vulkan::PipelineCache& cache_, SyntheticGnmArena& arena_)
        : cache{cache_}, arena{arena_} {
        dummy_vs_info.hw_stage = Shader::HwStage::Vertex;
        dummy_vs_info.sw_stage = Shader::SwStage::Vertex;
        dummy_fs_info.hw_stage = Shader::HwStage::Fragment;
        dummy_fs_info.sw_stage = Shader::SwStage::Fragment;
        dummy_fs_info.mrt_mask = 1u;
        dummy_vs_rt.Initialize(Shader::HwStage::Vertex, Shader::SwStage::Vertex);
        dummy_fs_rt.Initialize(Shader::HwStage::Fragment, Shader::SwStage::Fragment);
        passthrough_fs_spv = EmitPassthroughFragmentSpirv();
    }

    const std::vector<u32>& GetPassthroughVs(u32 num_outputs) {
        const u32 clamped = std::min<u32>(num_outputs, 32u);
        if (passthrough_vs_spv[clamped].empty()) {
            passthrough_vs_spv[clamped] = EmitPassthroughVertexSpirv(clamped);
        }
        return passthrough_vs_spv[clamped];
    }
};

void BuildFragmentRuntimeAndKey(const ExtractedShader& sh,
                                const std::vector<std::pair<u8, u8>>* vs_exports,
                                Shader::RuntimeInfo& rt,
                                Vulkan::GraphicsPipelineKey& key,
                                u32& max_param_loc) {
    rt = {};
    rt.Initialize(Shader::HwStage::Fragment, Shader::SwStage::Fragment);
    AmdGpu::ShaderProgram pgm{};
    std::memcpy(&pgm.settings, sh.regs.data() + 2, 8);
    rt.props = {
        pgm.settings.fp_denorm_mode32,
        pgm.settings.fp_denorm_mode64,
        pgm.settings.fp_round_mode32,
        pgm.settings.fp_round_mode64,
        std::min<u32>(pgm.settings.num_user_regs, 16u),
        pgm.settings.vgpr_comp_cnt,
        pgm.NumVgprs(),
    };
    auto& fs = rt.hw.fs;
    fs.en_flags = std::bit_cast<AmdGpu::PsInput>(sh.regs[6]);
    fs.addr_flags = std::bit_cast<AmdGpu::PsInput>(sh.regs[7]);
    fs.front_face_all_bits =
        std::bit_cast<AmdGpu::BarycentricControl>(sh.regs[9]).front_face_all_bits;
    fs.z_export_format = static_cast<AmdGpu::ShaderExportFormat>(sh.regs[4] & 0xFu);
    const auto depth = std::bit_cast<AmdGpu::DepthShaderControl>(sh.regs[10]);
    fs.mrtz_mask =
        depth.z_export_enable |
        ((depth.stencil_op_val_export_enable || depth.stencil_test_val_export_enable) << 1) |
        (depth.mask_export_enable << 2) | (depth.coverage_to_mask_enable << 3);

    const u32 num_in = std::min<u32>(static_cast<u32>(sh.semantics.size()), 32u);
    fs.num_inputs = num_in;
    max_param_loc = 0;
    for (u32 i = 0; i < num_in; ++i) {
        const u32 raw_sem = sh.semantics[i];
        const bool flat = (raw_sem & 0x0400u) != 0;
        if (vs_exports) {
            const u8 sem_id = static_cast<u8>(raw_sem & 0xFFu);
            auto it = std::ranges::find_if(*vs_exports,
                                           [sem_id](const auto& kv) { return kv.first == sem_id; });
            if (it != vs_exports->end() && it->second < 32u) {
                fs.inputs[i] = {it->second, false, flat, 0};
                max_param_loc = std::max<u32>(max_param_loc, u32(it->second) + 1u);
            } else {
                fs.inputs[i] = {0, true, flat, static_cast<u8>((raw_sem >> 8) & 3u)};
            }
        } else {
            fs.inputs[i] = {static_cast<u8>(i), false, flat, 0};
            max_param_loc = std::max<u32>(max_param_loc, i + 1u);
        }
    }

    key = {};
    key.prim_type = AmdGpu::PrimitiveType::TriangleList;
    key.polygon_mode = AmdGpu::PolygonMode::Fill;
    key.z_format = AmdGpu::DepthBuffer::ZFormat::Z32Float;
    if (fs.mrtz_mask & 2u) {
        key.stencil_format = AmdGpu::DepthBuffer::StencilFormat::Stencil8;
    }
    key.depth_clip_enable = 1;
    key.num_samples = 1;
    key.depth_samples = 1;
    key.color_samples.fill(1);

    for (u32 cb = 0; cb < Shader::MaxColorBuffers; ++cb) {
        const u32 exp_fmt = (sh.regs[5] >> (cb * 4)) & 0xFu;
        if (exp_fmt == 0) {
            continue;
        }
        const auto sef = static_cast<AmdGpu::ShaderExportFormat>(exp_fmt);
        auto& col = fs.color_buffers[cb];
        if (sef == AmdGpu::ShaderExportFormat::ABGR_FP16 ||
            sef == AmdGpu::ShaderExportFormat::ABGR_32) {
            col.data_format = AmdGpu::DataFormat::Format16_16_16_16;
            col.num_format = AmdGpu::NumberFormat::Float;
        } else {
            col.data_format = AmdGpu::DataFormat::Format8_8_8_8;
            col.num_format = AmdGpu::NumberFormat::Unorm;
        }
        col.export_format = sef;
        col.swizzle = {.r = AmdGpu::CompSwizzle::Red,
                       .g = AmdGpu::CompSwizzle::Green,
                       .b = AmdGpu::CompSwizzle::Blue,
                       .a = AmdGpu::CompSwizzle::Alpha};
        key.color_buffers[cb] = col;
        key.write_masks[cb] =
            vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    }
}

bool PopulateVertexInputState(const std::optional<Shader::Gcn::FetchShaderData>& fetch,
                              Vulkan::GraphicsPipelineKey& key,
                              Vulkan::GraphicsPipeline::SerializationSupport& sdata) {
    if (!fetch) {
        return true;
    }
    if (fetch->attributes.size() > kMaxPrecompileVertexAttribs) {
        return false;
    }
    u32 slot = 0;
    for (const auto& attrib : fetch->attributes) {
        if (attrib.semantic >= kMaxPrecompileVertexAttribs ||
            slot >= Vulkan::MaxVertexBufferCount) {
            return false;
        }
        key.vertex_buffer_formats[slot] = vk::Format::eR32G32B32A32Sfloat;
        sdata.vertex_bindings.push_back(vk::VertexInputBindingDescription{
            .binding = slot,
            .stride = 16,
            .inputRate = vk::VertexInputRate::eVertex,
        });
        sdata.vertex_attributes.push_back(vk::VertexInputAttributeDescription{
            .location = attrib.semantic,
            .binding = slot,
            .format = vk::Format::eR32G32B32A32Sfloat,
            .offset = 0,
        });
        ++slot;
    }
    return true;
}

bool ValidateSpirvModule(std::span<const u32> spv, u64 shader_hash) noexcept {
    if (spv.size() < 6 || spv[0] != 0x07230203u) {
        return false;
    }
    const u32 bound = spv[3];
    if (bound < 2u || bound > (1u << 22)) {
        return false;
    }
    auto ok_id = [bound](u32 id) noexcept { return id != 0u && id < bound; };

    size_t pos = 5;
    while (pos < spv.size()) {
        const u32 hdr = spv[pos];
        const u32 wc = hdr >> 16;
        const u32 op = hdr & 0xFFFFu;
        if (wc == 0 || pos + wc > spv.size()) {
            std::printf("GPU: rejected truncated SPIR-V (op=%u wc=%u pos=%zu) for %016llx\n", op,
                        wc, pos, static_cast<unsigned long long>(shader_hash));
            return false;
        }
        auto fail_at = [&](u32 slot) {
            std::printf("GPU: rejected invalid SPIR-V ID=%u (bound=%u op=%u slot=%u) for %016llx\n",
                        spv[pos + slot], bound, op, slot,
                        static_cast<unsigned long long>(shader_hash));
            return false;
        };
        auto check_range = [&](u32 first_slot, u32 end_slot) {
            for (u32 s = first_slot; s < end_slot && s < wc; ++s) {
                if (!ok_id(spv[pos + s])) {
                    return fail_at(s);
                }
            }
            return true;
        };

        switch (op) {
        case 12: // OpExtInst: Type, Result, Set, LiteralInst, Operands...
            if (wc < 4 || !check_range(1, 4) || !check_range(5, wc)) return false;
            break;
        case 15: // OpEntryPoint: ExecModel, EntryPointId, Name..., InterfaceIds...
            if (wc < 4 || !ok_id(spv[pos + 2])) return fail_at(2);
            break;
        case 16: // OpExecutionMode: EntryPointId, Mode, ...
        case 71: // OpDecorate: TargetId, ...
        case 72: // OpMemberDecorate: StructTypeId, ...
        case 247: // OpSelectionMerge: MergeBlockId, Control
        case 248: // OpLabel: ResultId
        case 249: // OpBranch: TargetLabelId
            if (wc < 2 || !ok_id(spv[pos + 1])) return fail_at(1);
            break;
        case 59: // OpVariable: Type, Result, StorageClass, [Initializer]
            if (wc < 4 || !check_range(1, 3) || (wc >= 5 && !ok_id(spv[pos + 4]))) return false;
            break;
        case 61: // OpLoad: Type, Result, Pointer, [MemoryAccess...]
        case 81: // OpCompositeExtract: Type, Result, Composite, Indexes...
        case 250: // OpBranchConditional: Cond, TrueLabel, FalseLabel, [Weights]
            if (wc < 4 || !check_range(1, 4)) return false;
            break;
        case 62: // OpStore: Pointer, Object, [MemoryAccess...]
        case 246: // OpLoopMerge: MergeBlock, ContinueTarget, Control
        case 251: // OpSwitch: Selector, Default, ...
            if (wc < 3 || !check_range(1, 3)) return false;
            break;
        case 79: // OpVectorShuffle: Type, Result, Vec1, Vec2, Components...
        case 82: // OpCompositeInsert: Type, Result, Object, Composite, Indexes...
            if (wc < 5 || !check_range(1, 5)) return false;
            break;
        case 57: // OpFunctionCall
        case 65: // OpAccessChain
        case 66: // OpInBoundsAccessChain
        case 80: // OpCompositeConstruct
        case 83: // OpCopyObject
        case 84: // OpTranspose
        case 245: // OpPhi
            if (wc < 3 || !check_range(1, wc)) return false;
            break;
        case 99: // OpImageWrite: Image, Coordinate, Texel, ...
            if (wc < 4 || !check_range(1, 4)) return false;
            break;
        default:
            if ((op >= 87 && op <= 98) || op == 100 || op == 101) {
                // OpImageSample* / OpImageFetch / OpImageRead / OpImageGather: Type, Result, Img, Coord
                if (wc < 5 || !check_range(1, 5)) return false;
            } else if ((op >= 102 && op <= 107) || (op >= 109 && op <= 124) ||
                       (op >= 126 && op <= 191) || (op >= 201 && op <= 205)) {
                // Pure ID-operand image queries, conversions, arithmetic, logic, comparisons, bit ops
                if (wc < 3 || !check_range(1, wc)) return false;
            }
            break;
        }
        pos += wc;
    }
    return true;
}

SHAD_NO_INLINE bool CompileSingleExtractedInner(PrecompileContext& ctx,
                                                const ExtractedShader& sh) {
    ScopedSoftAssert soft_assert;
    try {
        std::array<u32, 16> user_regs{};
        if (!ctx.arena.Populate(sh, user_regs)) {
            return false;
        }

        const Shader::Profile& profile = ctx.cache.GetProfile();
        Shader::Pools pools;

        if (sh.kind == StageKind::Compute) {
            Shader::RuntimeInfo rt{};
            rt.Initialize(Shader::HwStage::Compute, Shader::SwStage::Compute);
            AmdGpu::ComputeProgram cs_pgm{};
            std::memcpy(&cs_pgm.settings, sh.regs.data() + 3, 8);
            std::memcpy(&cs_pgm.num_thread_x, sh.regs.data() + 5, 12);
            rt.props.fp_denorm_mode32 = cs_pgm.settings.fp_denorm_mode32;
            rt.props.fp_denorm_mode16_64 = cs_pgm.settings.fp_denorm_mode64;
            rt.props.fp_round_mode32 = cs_pgm.settings.fp_round_mode32;
            rt.props.fp_round_mode16_64 = cs_pgm.settings.fp_round_mode64;
            rt.props.num_user_data = std::min<u32>(cs_pgm.settings.num_user_regs, 16u);
            rt.props.num_allocated_vgprs = cs_pgm.settings.num_vgprs * 4;
            rt.hw.cs.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                       cs_pgm.num_thread_z.full};
            rt.hw.cs.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                                    cs_pgm.IsTgidEnabled(2)};
            rt.hw.cs.shared_memory_size = cs_pgm.SharedMemSize();

            Shader::ShaderParams params{
                .user_data = user_regs,
                .code = sh.code,
                .hash = sh.hash,
            };
            Shader::Info info{Shader::HwStage::Compute, Shader::SwStage::Compute, params};
            Shader::Backend::Bindings binding{};
            auto ir = Shader::TranslateProgram(sh.code, pools, info, rt, profile);
            auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, rt, ir, binding);
            pools.ReleaseContents();
            if (!ValidateSpirvModule(spv, sh.hash)) {
                return false;
            }
            return ctx.cache.PrecompileArchiveCompute(info, spv);
        }

        if (sh.kind == StageKind::Fragment) {
            Shader::RuntimeInfo rt{};
            Vulkan::GraphicsPipelineKey key{};
            u32 max_param_loc = 0;
            BuildFragmentRuntimeAndKey(sh, nullptr, rt, key, max_param_loc);

            Shader::ShaderParams params{
                .user_data = user_regs,
                .code = sh.code,
                .hash = sh.hash,
            };
            Shader::Info info{Shader::HwStage::Fragment, Shader::SwStage::Fragment, params};
            Shader::Backend::Bindings binding{};
            auto ir = Shader::TranslateProgram(sh.code, pools, info, rt, profile);
            if (!info.fmasks.empty()) {
                return false;
            }
            auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, rt, ir, binding);
            pools.ReleaseContents();
            if (!ValidateSpirvModule(spv, sh.hash)) {
                return false;
            }

            key.mrt_mask = info.mrt_mask;
            key.num_color_attachments = std::bit_width(info.mrt_mask);
            for (u32 cb = 0; cb < Shader::MaxColorBuffers; ++cb) {
                if ((key.mrt_mask & (1u << cb)) == 0) {
                    std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
                    key.write_masks[cb] = {};
                }
            }

            Vulkan::GraphicsPipeline::SerializationSupport sdata{};
            sdata.multisampling.rasterizationSamples = vk::SampleCountFlagBits::e1;
            const auto& vs_spv = ctx.GetPassthroughVs(max_param_loc);
            return ctx.cache.PrecompileArchiveGraphics(&ctx.dummy_vs_info, ctx.dummy_vs_rt, vs_spv,
                                                       std::nullopt, &info, rt, spv, key, sdata);
        }

        if (sh.kind == StageKind::Vertex) {
            Shader::RuntimeInfo rt{};
            rt.Initialize(Shader::HwStage::Vertex, Shader::SwStage::Vertex);
            AmdGpu::ShaderProgram pgm{};
            std::memcpy(&pgm.settings, sh.regs.data() + 2, 8);
            rt.props = {
                pgm.settings.fp_denorm_mode32,
                pgm.settings.fp_denorm_mode64,
                pgm.settings.fp_round_mode32,
                pgm.settings.fp_round_mode64,
                std::min<u32>(pgm.settings.num_user_regs, 16u),
                pgm.settings.vgpr_comp_cnt,
                pgm.NumVgprs(),
            };
            u32 mapped_out = 0;
            if (!MapVsOutputsSafe(rt.hw.vs.outputs,
                                  std::bit_cast<AmdGpu::VsOutputControl>(sh.regs[6]), mapped_out)) {
                return false;
            }
            rt.hw.vs.num_outputs = mapped_out;
            rt.sw.vs.step_rate_0 = 1;
            rt.sw.vs.step_rate_1 = 1;

            Shader::ShaderParams params{
                .user_data = user_regs,
                .code = sh.code,
                .hash = sh.hash,
            };
            Shader::Info info{Shader::HwStage::Vertex, Shader::SwStage::Vertex, params};
            Shader::Backend::Bindings binding{};
            auto ir = Shader::TranslateProgram(sh.code, pools, info, rt, profile);
            if (!info.fmasks.empty()) {
                return false;
            }
            const auto fetch = Shader::Gcn::ParseFetchShader(info);
            auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, rt, ir, binding);
            pools.ReleaseContents();
            if (!ValidateSpirvModule(spv, sh.hash)) {
                return false;
            }

            Vulkan::GraphicsPipelineKey key{};
            key.prim_type = AmdGpu::PrimitiveType::TriangleList;
            key.polygon_mode = AmdGpu::PolygonMode::Fill;
            key.z_format = AmdGpu::DepthBuffer::ZFormat::Z32Float;
            key.depth_clip_enable = 1;
            key.num_samples = 1;
            key.depth_samples = 1;
            key.color_samples.fill(1);
            key.mrt_mask = 0u;
            key.num_color_attachments = 0u;

            Vulkan::GraphicsPipeline::SerializationSupport sdata{};
            sdata.multisampling.rasterizationSamples = vk::SampleCountFlagBits::e1;
            if (!PopulateVertexInputState(fetch, key, sdata)) {
                return false;
            }

            // Depth-only / standalone VS pipeline: caches VS NIR + depth/shadow VS DXIL cleanly.
            return ctx.cache.PrecompileArchiveGraphics(&info, rt, spv, fetch, nullptr,
                                                       ctx.dummy_fs_rt, {}, key, sdata);
        }
    } catch (...) {
        return false;
    }
    return false;
}

SHAD_NO_INLINE bool CompileLinkedPairInner(PrecompileContext& ctx,
                                           const ExtractedShader& vs_sh,
                                           const ExtractedShader& ps_sh) {
    ScopedSoftAssert soft_assert;
    try {
        const Shader::Profile& profile = ctx.cache.GetProfile();
        Shader::Backend::Bindings binding{};

        // 1. Translate + Emit Fragment FIRST at binding = {0,0,0} (exact RefreshGraphicsStages order!).
        std::array<u32, 16> ps_user_regs{};
        if (!ctx.arena.Populate(ps_sh, ps_user_regs)) {
            return false;
        }
        Shader::RuntimeInfo ps_rt{};
        Vulkan::GraphicsPipelineKey key{};
        u32 max_param_loc = 0;
        BuildFragmentRuntimeAndKey(ps_sh, &vs_sh.vs_exports, ps_rt, key, max_param_loc);

        Shader::ShaderParams ps_params{
            .user_data = ps_user_regs,
            .code = ps_sh.code,
            .hash = ps_sh.hash,
        };
        Shader::Info ps_info{Shader::HwStage::Fragment, Shader::SwStage::Fragment, ps_params};
        std::vector<u32> ps_spv;
        {
            Shader::Pools ps_pools;
            auto ps_ir = Shader::TranslateProgram(ps_sh.code, ps_pools, ps_info, ps_rt, profile);
            if (!ps_info.fmasks.empty()) {
                return false;
            }
            ps_spv = Shader::Backend::SPIRV::EmitSPIRV(profile, ps_rt, ps_ir, binding);
            ps_pools.ReleaseContents();
            if (!ValidateSpirvModule(ps_spv, ps_sh.hash)) {
                return false;
            }
        }

        // 2. Translate + Emit Vertex SECOND with post-Fragment `binding` (exact runtime match!).
        std::array<u32, 16> vs_user_regs{};
        if (!ctx.arena.Populate(vs_sh, vs_user_regs)) {
            return false;
        }
        Shader::RuntimeInfo vs_rt{};
        vs_rt.Initialize(Shader::HwStage::Vertex, Shader::SwStage::Vertex);
        AmdGpu::ShaderProgram vs_pgm{};
        std::memcpy(&vs_pgm.settings, vs_sh.regs.data() + 2, 8);
        vs_rt.props = {
            vs_pgm.settings.fp_denorm_mode32,
            vs_pgm.settings.fp_denorm_mode64,
            vs_pgm.settings.fp_round_mode32,
            vs_pgm.settings.fp_round_mode64,
            std::min<u32>(vs_pgm.settings.num_user_regs, 16u),
            vs_pgm.settings.vgpr_comp_cnt,
            vs_pgm.NumVgprs(),
        };
        u32 mapped_out = 0;
        if (!MapVsOutputsSafe(vs_rt.hw.vs.outputs,
                              std::bit_cast<AmdGpu::VsOutputControl>(vs_sh.regs[6]), mapped_out)) {
            return false;
        }
        vs_rt.hw.vs.num_outputs = mapped_out;
        vs_rt.sw.vs.step_rate_0 = 1;
        vs_rt.sw.vs.step_rate_1 = 1;

        Shader::ShaderParams vs_params{
            .user_data = vs_user_regs,
            .code = vs_sh.code,
            .hash = vs_sh.hash,
        };
        Shader::Info vs_info{Shader::HwStage::Vertex, Shader::SwStage::Vertex, vs_params};
        std::optional<Shader::Gcn::FetchShaderData> fetch;
        std::vector<u32> vs_spv;
        {
            Shader::Pools vs_pools;
            auto vs_ir = Shader::TranslateProgram(vs_sh.code, vs_pools, vs_info, vs_rt, profile);
            if (!vs_info.fmasks.empty()) {
                return false;
            }
            fetch = Shader::Gcn::ParseFetchShader(vs_info);
            vs_spv = Shader::Backend::SPIRV::EmitSPIRV(profile, vs_rt, vs_ir, binding);
            vs_pools.ReleaseContents();
            if (!ValidateSpirvModule(vs_spv, vs_sh.hash)) {
                return false;
            }
        }

        if (u64(ps_info.buffers.size() + vs_info.buffers.size()) > 30ull) {
            return false;
        }

        key.mrt_mask = ps_info.mrt_mask;
        key.num_color_attachments = std::bit_width(ps_info.mrt_mask);
        for (u32 cb = 0; cb < Shader::MaxColorBuffers; ++cb) {
            if ((key.mrt_mask & (1u << cb)) == 0) {
                std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
                key.write_masks[cb] = {};
            }
        }

        Vulkan::GraphicsPipeline::SerializationSupport sdata{};
        sdata.multisampling.rasterizationSamples = vk::SampleCountFlagBits::e1;
        if (!PopulateVertexInputState(fetch, key, sdata)) {
            return false;
        }

        return ctx.cache.PrecompileArchiveGraphics(&vs_info, vs_rt, vs_spv, fetch, &ps_info, ps_rt,
                                                   ps_spv, key, sdata);
    } catch (...) {
        return false;
    }
}

u32 WarmAuxiliaryGeometryShaders(PrecompileContext& ctx) {
    u32 ok = 0;
    for (const auto prim : {AmdGpu::PrimitiveType::RectList, AmdGpu::PrimitiveType::QuadList}) {
        try {
            const auto aux_type = (prim == AmdGpu::PrimitiveType::QuadList)
                                      ? Shader::Backend::SPIRV::AuxShaderType::QuadListGS
                                      : Shader::Backend::SPIRV::AuxShaderType::RectListGS;
            for (const u32 num_attrs : {0u, 1u, 2u}) {
                Shader::RuntimeInfo fs_rt = ctx.dummy_fs_rt;
                fs_rt.hw.fs.num_inputs = num_attrs;
                for (u32 i = 0; i < num_attrs; ++i) {
                    fs_rt.hw.fs.inputs[i] = {static_cast<u8>(i), false, false, 0};
                }
                Vulkan::GraphicsPipelineKey key{};
                key.prim_type = prim;
                key.polygon_mode = AmdGpu::PolygonMode::Fill;
                key.z_format = AmdGpu::DepthBuffer::ZFormat::Z32Float;
                key.depth_clip_enable = 1;
                key.num_samples = 1;
                key.depth_samples = 1;
                key.color_samples.fill(1);
                key.mrt_mask = 1u;
                key.num_color_attachments = 1u;
                key.color_buffers[0] = {
                    .data_format = AmdGpu::DataFormat::Format8_8_8_8,
                    .num_format = AmdGpu::NumberFormat::Unorm,
                    .export_format = AmdGpu::ShaderExportFormat::ABGR_FP16,
                    .swizzle = {.r = AmdGpu::CompSwizzle::Red,
                                .g = AmdGpu::CompSwizzle::Green,
                                .b = AmdGpu::CompSwizzle::Blue,
                                .a = AmdGpu::CompSwizzle::Alpha},
                };
                key.write_masks[0] =
                    vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                    vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

                Vulkan::GraphicsPipeline::SerializationSupport sdata{};
                sdata.multisampling.rasterizationSamples = vk::SampleCountFlagBits::e1;
                sdata.tcs = Shader::Backend::SPIRV::EmitAuxilaryTessShader(aux_type, fs_rt.hw.fs);
                const auto& vs_spv = ctx.GetPassthroughVs(num_attrs);
                if (ctx.cache.PrecompileArchiveGraphics(&ctx.dummy_vs_info, ctx.dummy_vs_rt, vs_spv,
                                                        std::nullopt, &ctx.dummy_fs_info, fs_rt,
                                                        ctx.passthrough_fs_spv, key, sdata)) {
                    ++ok;
                }
            }
        } catch (...) {
        }
    }
    return ok;
}

bool CompileSingleExtractedSafe(PrecompileContext* ctx, const ExtractedShader* sh) {
    const size_t walker_mark = Shader::GetUwpSrtWalkerWatermark();
    const bool ok = CompileSingleExtractedInner(*ctx, *sh);
    Shader::RewindUwpSrtWalkerTo(walker_mark);
    return ok;
}

bool CompileLinkedPairSafe(PrecompileContext* ctx, const ExtractedShader* vs_sh,
                           const ExtractedShader* ps_sh) {
    const size_t walker_mark = Shader::GetUwpSrtWalkerWatermark();
    const bool ok = CompileLinkedPairInner(*ctx, *vs_sh, *ps_sh);
    Shader::RewindUwpSrtWalkerTo(walker_mark);
    return ok;
}

} // namespace

ActiveDiagnostic GetActiveShaderDiagnostic() noexcept {
    ActiveDiagnostic d{};
    d.active = g_diag_active.load(std::memory_order_acquire);
    if (d.active) {
        d.index = g_diag_index.load(std::memory_order_relaxed);
        d.total = g_diag_total.load(std::memory_order_relaxed);
        d.stage = g_diag_stage.load(std::memory_order_relaxed);
        d.hash = g_diag_hash.load(std::memory_order_relaxed);
        d.partner_hash = g_diag_partner_hash.load(std::memory_order_relaxed);
    }
    return d;
}

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
    if (local.empty()) {
        return 0;
    }
    u32 exported = 0;
    const auto portable = LocalPortableCacheRoot();
    if (!portable.empty()) {
        exported += SyncDirectoryTree(local, portable, {}, "Exporting shader cache...", true);
    }
    const auto dump_cache = DumpPortableCacheRoot();
    if (!dump_cache.empty()) {
        SyncDirectoryTree(local, dump_cache, {}, "Exporting USB shader cache...", true);
    }
    if (exported > 0) {
        std::printf("GPU: exported %u shader/pipeline cache files to %s\n", exported,
                    portable.string().c_str());
    }
    return exported;
}

ScanSummary ScanGameShaderArchives(const ProgressFn& progress) {
    ScanSummary summary{};
    const auto archives = FindShaderBndArchives();
    summary.archives_found = static_cast<u32>(archives.size());

    std::unordered_set<u64> unique_hashes;
    for (u32 idx = 0; idx < summary.archives_found; ++idx) {
        const auto& path = archives[idx];
        if (progress) {
            progress("Verifying " + path.filename().string() + "...", idx,
                     std::max(summary.archives_found, 1u));
        }
        std::vector<u8> payload;
        if (!LoadShaderContainer(path, payload)) {
            continue;
        }
        ++summary.archives_ok;

        ForEachContainerSlice(payload, [&](std::span<const u8> slice, const std::string&) {
            for (size_t at = 0; at + 8 <= slice.size(); ++at) {
                const auto prog = ParseOrbProgramAt(slice, at);
                if (!prog) {
                    continue;
                }
                ++summary.total_orbshdr;
                unique_hashes.insert(prog->hash);
                switch (prog->type) {
                case 0: ++summary.ps_count; break;
                case 1: ++summary.vs_count; break;
                case 3: ++summary.cs_count; break;
                default: ++summary.gs_hs_ds_count; break;
                }
                at = prog->footer + 27;
            }
        });
        payload.clear();
        payload.shrink_to_fit();
        CompactHostHeaps();
    }

    if (const auto boot_bin = FindPreparedBootBin(); !boot_bin.empty()) {
        Common::FS::IOFile bf{boot_bin, Common::FS::FileAccessMode::Read};
        if (bf.IsOpen() && bf.GetSize() <= 160ull * 1024 * 1024) {
            std::vector<u8> img(static_cast<size_t>(bf.GetSize()));
            if (bf.ReadRaw<u8>(img.data(), img.size()) == img.size()) {
                bf.Close();
                for (size_t at = 0; at + 8 <= img.size(); ++at) {
                    const auto prog = ParseOrbProgramAt(img, at);
                    if (!prog) {
                        continue;
                    }
                    ++summary.boot_orbshdr;
                    ++summary.total_orbshdr;
                    unique_hashes.insert(prog->hash);
                    switch (prog->type) {
                    case 0: ++summary.ps_count; break;
                    case 1: ++summary.vs_count; break;
                    case 3: ++summary.cs_count; break;
                    default: ++summary.gs_hs_ds_count; break;
                    }
                    at = prog->footer + 27;
                }
            }
        }
        CompactHostHeaps();
    }

    summary.unique_hashes = static_cast<u32>(unique_hashes.size());
    if (progress) {
        progress("Verified Bloodborne shader archives", summary.archives_ok,
                 std::max(summary.archives_found, 1u));
    }
    std::printf("GPU: verified %u/%u shader archives (+%u boot.bin OrbShdr): %u OrbShdr binaries "
                "(%u unique: VS=%u PS=%u CS=%u GS/HS/DS/ES/LS=%u)\n",
                summary.archives_ok, summary.archives_found, summary.boot_orbshdr,
                summary.total_orbshdr, summary.unique_hashes, summary.vs_count, summary.ps_count,
                summary.cs_count, summary.gs_hs_ds_count);
    std::fflush(stdout);
    return summary;
}

ScanSummary PrecompileGameShaderArchives(Vulkan::Rasterizer& rasterizer,
                                         const ProgressFn& progress) {
    ScanSummary summary{};
    const auto archives = FindShaderBndArchives();
    summary.archives_found = static_cast<u32>(archives.size());

    std::unordered_set<u64> unique_hashes;
    std::unordered_map<u64, ExtractedShader> extracted_map;
    std::vector<u64> extracted_order;
    std::vector<std::pair<u64, u64>> linked_pairs;
    std::unordered_set<u64> seen_pair_keys;
    std::unordered_map<std::string, u64> stem_vs;
    std::unordered_map<std::string, u64> stem_ps;

    auto add_pair = [&](u64 vs_h, u64 ps_h) {
        if (vs_h == 0 || ps_h == 0 || linked_pairs.size() >= kMaxLinkedPairs) {
            return;
        }
        const u64 pair_key = vs_h ^ (ps_h * 0x9E3779B97F4A7C15ull + (ps_h << 6) + (ps_h >> 2));
        if (seen_pair_keys.insert(pair_key).second) {
            linked_pairs.emplace_back(vs_h, ps_h);
        }
    };

    auto scan_slice = [&](std::span<const u8> slice, const std::string& stem, bool is_boot) {
        std::optional<u64> last_vs_in_slice;
        std::optional<u64> first_ps_in_slice;
        for (size_t at = 0; at + 8 <= slice.size(); ++at) {
            const auto prog = ParseOrbProgramAt(slice, at);
            if (!prog) {
                continue;
            }
            if (is_boot) {
                ++summary.boot_orbshdr;
            }
            ++summary.total_orbshdr;
            const bool first_seen = unique_hashes.insert(prog->hash).second;
            switch (prog->type) {
            case 0: ++summary.ps_count; break;
            case 1: ++summary.vs_count; break;
            case 3: ++summary.cs_count; break;
            default: ++summary.gs_hs_ds_count; break;
            }
            if (prog->type == 0 || prog->type == 1 || prog->type == 3) {
                if (first_seen) {
                    ExtractedShader sh{};
                    bool ok = false;
                    if (prog->type == 3) {
                        ok = TryExtractCs(slice, *prog, sh);
                    } else if (prog->type == 0) {
                        ok = TryExtractPs(slice, *prog, sh);
                    } else if (prog->type == 1) {
                        ok = TryExtractVs(slice, *prog, sh);
                    }
                    if (ok && extracted_map.emplace(prog->hash, std::move(sh)).second) {
                        extracted_order.push_back(prog->hash);
                    }
                }
                if (extracted_map.contains(prog->hash)) {
                    if (prog->type == 1) {
                        last_vs_in_slice = prog->hash;
                        if (first_ps_in_slice) {
                            add_pair(prog->hash, *first_ps_in_slice);
                        }
                        if (!stem.empty()) {
                            stem_vs[stem] = prog->hash;
                            if (auto it = stem_ps.find(stem); it != stem_ps.end()) {
                                add_pair(prog->hash, it->second);
                            }
                        }
                    } else if (prog->type == 0) {
                        if (!first_ps_in_slice) {
                            first_ps_in_slice = prog->hash;
                        }
                        if (last_vs_in_slice) {
                            add_pair(*last_vs_in_slice, prog->hash);
                        }
                        if (!stem.empty()) {
                            stem_ps[stem] = prog->hash;
                            if (auto it = stem_vs.find(stem); it != stem_vs.end()) {
                                add_pair(it->second, prog->hash);
                            }
                        }
                    }
                }
            }
            at = prog->footer + 27;
        }
    };

    for (u32 idx = 0; idx < summary.archives_found; ++idx) {
        const auto& path = archives[idx];
        if (progress) {
            progress("Extracting " + path.filename().string() + "...", idx,
                     std::max(summary.archives_found, 1u));
        }
        std::vector<u8> payload;
        if (!LoadShaderContainer(path, payload)) {
            std::printf("GPU: warning: failed to load shader archive %s\n", path.string().c_str());
            continue;
        }
        ++summary.archives_ok;
        stem_vs.clear();
        stem_ps.clear();
        ForEachContainerSlice(payload, [&](std::span<const u8> slice, const std::string& stem) {
            scan_slice(slice, stem, false);
        });
        payload.clear();
        payload.shrink_to_fit();
        CompactHostHeaps();
    }

    if (const auto boot_bin = FindPreparedBootBin(); !boot_bin.empty()) {
        Common::FS::IOFile bf{boot_bin, Common::FS::FileAccessMode::Read};
        if (bf.IsOpen() && bf.GetSize() <= 160ull * 1024 * 1024) {
            std::vector<u8> img(static_cast<size_t>(bf.GetSize()));
            if (bf.ReadRaw<u8>(img.data(), img.size()) == img.size()) {
                bf.Close();
                scan_slice(img, std::string{}, true);
            }
        }
        CompactHostHeaps();
    }

    summary.unique_hashes = static_cast<u32>(unique_hashes.size());
    std::printf("GPU: extracted %zu compilable CS/PS/VS programs (+%zu linked VS+PS pairs, "
                "%u boot.bin OrbShdr) from %u/%u shader archives "
                "(%u OrbShdr total, %u unique: VS=%u PS=%u CS=%u GS/HS/DS/ES/LS=%u)\n",
                extracted_order.size(), linked_pairs.size(), summary.boot_orbshdr,
                summary.archives_ok, summary.archives_found, summary.total_orbshdr,
                summary.unique_hashes, summary.vs_count, summary.ps_count, summary.cs_count,
                summary.gs_hs_ds_count);
    std::fflush(stdout);

    if (extracted_order.empty()) {
        return summary;
    }

    auto& pipe_cache = rasterizer.GetPipelineCache();
    Storage::DataBase::Instance().Open();
    Storage::DataBase::Instance().FinishPreload();

    SyntheticGnmArena arena;
    if (!arena.Valid()) {
        std::printf("GPU: warning: could not allocate < 1 TiB synthetic GNM precompile arena\n");
        return summary;
    }

    PrecompileContext ctx{pipe_cache, arena};
    ConsumeInflightCrashBreadcrumb();
    const auto quarantined = LoadQuarantineHashes();
    if (!quarantined.empty()) {
        std::printf("GPU: skipping %zu previously quarantined shader hash(es)\n",
                    quarantined.size());
    }

    // Compile CS first, then PS, then VS so compute & fragment NIR/DXIL land earliest.
    std::ranges::stable_sort(extracted_order, [&](u64 a, u64 b) {
        return static_cast<u8>(extracted_map[a].kind) < static_cast<u8>(extracted_map[b].kind);
    });

    const u32 standalone_total = static_cast<u32>(extracted_order.size());
    const u32 pairs_total = static_cast<u32>(linked_pairs.size());
    const u32 grand_total = standalone_total + pairs_total + 1u;

    InflightBreadcrumb inflight;
    {
        ScopedActiveDiag diag{0, grand_total, 4u, 0, 0};
        summary.aux_gs_ok = WarmAuxiliaryGeometryShaders(ctx);
    }

    for (u32 i = 0; i < standalone_total; ++i) {
        if ((i % 32u) == 0u && !HasSafeMemoryHeadroom()) {
            std::printf("GPU: stopping shader precompile early at %u/%u to preserve memory headroom\n",
                        i, grand_total);
            break;
        }
        auto it = extracted_map.find(extracted_order[i]);
        if (it == extracted_map.end()) {
            continue;
        }
        if (quarantined.contains(it->second.hash)) {
            ++summary.precompiled_skipped;
            continue;
        }
        ++summary.precompiled_attempted;
        {
            inflight.Record(i + 1u, static_cast<u32>(it->second.kind), it->second.hash, 0);
            ScopedActiveDiag diag{i + 1u, grand_total, static_cast<u32>(it->second.kind),
                                  it->second.hash, 0};
            if (CompileSingleExtractedSafe(&ctx, &it->second)) {
                ++summary.precompiled_ok;
            } else {
                ++summary.precompiled_skipped;
            }
        }

        if ((i % 8u) == 0u || i + 1 == standalone_total) {
            if (progress) {
                char label[192]{};
                std::snprintf(label, sizeof(label),
                              "Precompiling Bloodborne shaders (%u / %u, DXIL cached: %u)...",
                              i + 1u, grand_total, summary.precompiled_ok);
                progress(label, i + 1u, grand_total);
            }
        }
        if ((i % 32u) == 31u) {
            CompactHostHeaps();
        }
        if ((i % 256u) == 255u) {
            pipe_cache.FlushCaches();
        }
    }

    for (u32 p = 0; p < pairs_total; ++p) {
        const u32 step = standalone_total + p;
        if ((p % 32u) == 0u && !HasSafeMemoryHeadroom()) {
            std::printf("GPU: stopping linked VS+PS precompile early at %u/%u to preserve memory headroom\n",
                        step, grand_total);
            break;
        }
        const auto [vs_h, ps_h] = linked_pairs[p];
        const u64 pair_q = vs_h ^ (ps_h * 0x9E3779B97F4A7C15ull);
        if (quarantined.contains(vs_h) || quarantined.contains(ps_h) ||
            quarantined.contains(pair_q)) {
            continue;
        }
        auto vs_it = extracted_map.find(vs_h);
        auto ps_it = extracted_map.find(ps_h);
        if (vs_it == extracted_map.end() || ps_it == extracted_map.end()) {
            continue;
        }
        {
            inflight.Record(step + 1u, 3u, vs_h, ps_h);
            ScopedActiveDiag diag{step + 1u, grand_total, 3u, vs_h, ps_h};
            if (CompileLinkedPairSafe(&ctx, &vs_it->second, &ps_it->second)) {
                ++summary.linked_pairs_ok;
            }
        }
        if ((p % 8u) == 0u || p + 1 == pairs_total) {
            if (progress) {
                char label[192]{};
                std::snprintf(
                    label, sizeof(label),
                    "Linking Bloodborne VS+PS pairs (%u / %u, linked: %u)...",
                    step + 1u, grand_total, summary.linked_pairs_ok);
                progress(label, step + 1u, grand_total);
            }
        }
        if ((p % 32u) == 31u) {
            CompactHostHeaps();
        }
        if ((p % 256u) == 255u) {
            pipe_cache.FlushCaches();
        }
    }

    inflight.Clear();
    extracted_map.clear();
    linked_pairs.clear();
    CompactHostHeaps();
    pipe_cache.FlushCaches();
    ExportLocalCacheToPortable();

    const size_t vkcache_bytes = pipe_cache.GetDriverCacheBytes();
    std::printf("GPU: ahead-of-time DXIL precompile finished: "
                "%u/%u standalone CS/PS/VS cached (%u skipped), %u/%zu linked VS+PS pairs cached, "
                "%u aux GS cached, driver.vkcache = %.2f MiB\n",
                summary.precompiled_ok, summary.precompiled_attempted,
                summary.precompiled_skipped, summary.linked_pairs_ok,
                static_cast<size_t>(pairs_total), summary.aux_gs_ok,
                double(vkcache_bytes) / (1024.0 * 1024.0));
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

    ConsumeInflightCrashBreadcrumb();
    SyncPortableCacheToLocal(ui_progress);

    if (const char* scan_env = std::getenv("BB_SCAN_SHADERBND");
        scan_env && *scan_env == '1') {
        PrecompileGameShaderArchives(rasterizer, ui_progress);
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

    ConsumeInflightCrashBreadcrumb();
    const u32 imported = SyncPortableCacheToLocal(ui_progress);

    u32 warmed_ok = 0;
    u32 warmed_total = 0;
    rasterizer.GetPipelineCache().WarmUp([&](u32 done, u32 total, u32 ok) {
        warmed_total = total;
        warmed_ok = ok;
        char buf[160]{};
        std::snprintf(buf, sizeof(buf),
                      "Warming up saved PSOs (%u / %u, ready: %u)...", done, total, ok);
        ui_progress(buf, done, total);
    });

    const ScanSummary scan = PrecompileGameShaderArchives(rasterizer, ui_progress);
    const u32 exported = ExportLocalCacheToPortable();

    BbOverlay::SetLoading(false, 0, 0);
    if (present_frame) {
        present_frame();
    }
    CompactHostHeaps();

    const double cache_mib =
        double(rasterizer.GetPipelineCache().GetDriverCacheBytes()) / (1024.0 * 1024.0);
    char summary[400]{};
    std::snprintf(
        summary, sizeof(summary),
        "Precompiled: %u/%u archives (%u CS/PS/VS + %u VS+PS pairs / %u unique) | "
        "Warm-up: %u/%u PSOs | Cache: %.1f MiB",
        scan.archives_ok, scan.archives_found, scan.precompiled_ok, scan.linked_pairs_ok,
        scan.unique_hashes, warmed_ok, warmed_total, cache_mib);
    return summary;
}

} // namespace BbPrecompile
