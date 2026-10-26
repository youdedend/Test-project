// SPDX-License-Identifier: GPL-2.0-or-later
// Xbox UWP shader cache sync, Bloodborne .shaderbnd.dcx OrbShdr ahead-of-time DXIL precompiler,
// and pre-game warm-up UI.

#include "bb_precompile.h"
#include "bbport_overlay.h"
#include "bbport_toggles.h"

#include <algorithm>
#include <array>
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
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/regs_depth.h"
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

bool CopyFileViaIoFile(const std::filesystem::path& src, const std::filesystem::path& dst) {
    Common::FS::IOFile in{src, Common::FS::FileAccessMode::Read};
    if (!in.IsOpen()) {
        return false;
    }
    const u64 size = in.GetSize();
    if (size == 0 || size > 128ull * 1024 * 1024) {
        return false;
    }
    {
        Common::FS::IOFile existing{dst, Common::FS::FileAccessMode::Read};
        if (existing.IsOpen()) {
            const u64 dst_size = existing.GetSize();
            if (dst_size == size) {
                return false;
            }
            if (dst.extension() == ".vkcache" && dst_size > size) {
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
                      const char* label_prefix) {
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
        // Fallback probe for USB FutureAccessList directories where directory_iterator is blocked.
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

std::vector<std::filesystem::path> FindShaderBndArchives() {
    const char* dump = std::getenv("BB_UWP_DUMP");
    if (!dump || !*dump) {
        return {};
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
    std::ranges::sort(archives);
    return archives;
}

struct OrbProgram {
    size_t start = 0;
    size_t footer = 0;
    u64 hash = 0;
    u32 bytes = 0;
    u32 type = 0;   // 0=PS, 1=VS, 2=GS, 3=CS, 4=HS, 5=DS
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
    Compute,
    Fragment,
    Vertex,
};

struct ExtractedShader {
    StageKind kind{};
    u64 hash = 0;
    bool is_srt = false;
    std::array<u32, 13> regs{};
    std::vector<u32> usages;
    std::vector<u32> semantics; // VS: packed semantics; PS: 0..num_interp-1
    u32 vs_outputs = 0;
    std::vector<u32> code;
};

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
            const u16 sem = ReadLe16(sem_table + i * 2);
            out.semantics[i] = sem;
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
        if ((counts >> 16) != 0 || inputs > 32 || outputs > 32) {
            continue;
        }
        const size_t expected =
            32 + size_t(p.usages) * 4 + size_t(inputs) * 4 + ((size_t(outputs) * 2 + 3) & ~size_t(3));
        if (size != expected || ((ReadLe32(h + 12) >> 1) & 31u) > 16u) {
            continue;
        }
        out = {};
        out.kind = StageKind::Vertex;
        out.hash = p.hash;
        out.is_srt = p.is_srt;
        out.vs_outputs = outputs;
        std::memcpy(out.regs.data(), h, 32);
        out.usages.resize(p.usages);
        if (p.usages > 0) {
            std::memcpy(out.usages.data(), h + 32, size_t(p.usages) * 4);
        }
        out.semantics.resize(inputs);
        if (inputs > 0) {
            std::memcpy(out.semantics.data(), h + 32 + size_t(p.usages) * 4, size_t(inputs) * 4);
        }
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

        // Pre-seed ExtendedUserData with valid leaf pointers so simple 1-hop SRT loads stay mapped.
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
        if (semantics.size() > 32) {
            return false;
        }
        // Rotate offset across 64 KiB fetch region so ParseFetchShader's address cache never stales.
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
            rt.props.num_user_data = cs_pgm.settings.num_user_regs;
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
            return ctx.cache.PrecompileArchiveCompute(info, spv);
        }

        if (sh.kind == StageKind::Fragment) {
            Shader::RuntimeInfo rt{};
            rt.Initialize(Shader::HwStage::Fragment, Shader::SwStage::Fragment);
            AmdGpu::ShaderProgram pgm{};
            std::memcpy(&pgm.settings, sh.regs.data() + 2, 8);
            rt.props = {
                pgm.settings.fp_denorm_mode32, pgm.settings.fp_denorm_mode64,
                pgm.settings.fp_round_mode32,  pgm.settings.fp_round_mode64,
                pgm.settings.num_user_regs,    pgm.settings.vgpr_comp_cnt,
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
            for (u32 i = 0; i < num_in; ++i) {
                fs.inputs[i] = {static_cast<u8>(i), false, (sh.semantics[i] & 0x0400u) != 0, 0};
            }

            Vulkan::GraphicsPipelineKey key{};
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
            const auto& vs_spv = ctx.GetPassthroughVs(num_in);
            return ctx.cache.PrecompileArchiveGraphics(&ctx.dummy_vs_info, ctx.dummy_vs_rt, vs_spv,
                                                       std::nullopt, &info, rt, spv, key, sdata);
        }

        if (sh.kind == StageKind::Vertex) {
            Shader::RuntimeInfo rt{};
            rt.Initialize(Shader::HwStage::Vertex, Shader::SwStage::Vertex);
            AmdGpu::ShaderProgram pgm{};
            std::memcpy(&pgm.settings, sh.regs.data() + 2, 8);
            rt.props = {
                pgm.settings.fp_denorm_mode32, pgm.settings.fp_denorm_mode64,
                pgm.settings.fp_round_mode32,  pgm.settings.fp_round_mode64,
                pgm.settings.num_user_regs,    pgm.settings.vgpr_comp_cnt,
                pgm.NumVgprs(),
            };
            rt.hw.vs.num_outputs = std::min<u32>(sh.vs_outputs, 32u);
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

            Vulkan::GraphicsPipelineKey key{};
            key.prim_type = AmdGpu::PrimitiveType::TriangleList;
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
            if (fetch) {
                const u32 attr_cnt =
                    std::min<u32>(static_cast<u32>(fetch->attributes.size()),
                                  Vulkan::MaxVertexBufferCount);
                for (u32 a = 0; a < attr_cnt; ++a) {
                    key.vertex_buffer_formats[a] = vk::Format::eR32G32B32A32Sfloat;
                    sdata.vertex_bindings.push_back(vk::VertexInputBindingDescription{
                        .binding = a,
                        .stride = 16,
                        .inputRate = vk::VertexInputRate::eVertex,
                    });
                    sdata.vertex_attributes.push_back(vk::VertexInputAttributeDescription{
                        .location = a,
                        .binding = a,
                        .format = vk::Format::eR32G32B32A32Sfloat,
                        .offset = 0,
                    });
                }
            }

            return ctx.cache.PrecompileArchiveGraphics(&info, rt, spv, fetch, &ctx.dummy_fs_info,
                                                       ctx.dummy_fs_rt, ctx.passthrough_fs_spv, key,
                                                       sdata);
        }
    } catch (...) {
        return false;
    }
    return false;
}

bool CompileSingleExtractedSafe(PrecompileContext* ctx, const ExtractedShader* sh) {
    const size_t walker_mark = Shader::GetUwpSrtWalkerWatermark();
    sigjmp_buf recover;
    bool ok = false;
    if (sigsetjmp(recover, 0) == 0) {
        runtime_fault_recover = &recover;
        ok = CompileSingleExtractedInner(*ctx, *sh);
        runtime_fault_recover = nullptr;
    } else {
        runtime_fault_recover = nullptr;
        g_soft_assert_scope = false;
        ok = false;
    }
    Shader::RewindUwpSrtWalkerTo(walker_mark);
    return ok;
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
    if (local.empty()) {
        return 0;
    }
    u32 exported = 0;
    const auto portable = LocalPortableCacheRoot();
    if (!portable.empty()) {
        exported += SyncDirectoryTree(local, portable, {}, "Exporting shader cache...");
    }
    const auto dump_cache = DumpPortableCacheRoot();
    if (!dump_cache.empty()) {
        SyncDirectoryTree(local, dump_cache, {}, "Exporting USB shader cache...");
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
    if (archives.empty()) {
        return summary;
    }

    std::unordered_set<u64> unique_hashes;
    for (u32 idx = 0; idx < summary.archives_found; ++idx) {
        const auto& path = archives[idx];
        if (progress) {
            progress("Verifying " + path.filename().string() + "...", idx,
                     summary.archives_found);
        }
        std::vector<u8> payload;
        if (!DecompressDcxDflt(path, payload)) {
            continue;
        }
        ++summary.archives_ok;

        for (size_t at = 0; at + 8 <= payload.size(); ++at) {
            const auto prog = ParseOrbProgramAt(payload, at);
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
        payload.clear();
        payload.shrink_to_fit();
        CompactHostHeaps();
    }
    summary.unique_hashes = static_cast<u32>(unique_hashes.size());
    if (progress) {
        progress("Verified Bloodborne shader archives", summary.archives_ok,
                 std::max(summary.archives_found, 1u));
    }
    std::printf("GPU: verified %u/%u .shaderbnd.dcx archives: %u OrbShdr binaries "
                "(%u unique: VS=%u PS=%u CS=%u GS/HS/DS=%u)\n",
                summary.archives_ok, summary.archives_found, summary.total_orbshdr,
                summary.unique_hashes, summary.vs_count, summary.ps_count, summary.cs_count,
                summary.gs_hs_ds_count);
    std::fflush(stdout);
    return summary;
}

ScanSummary PrecompileGameShaderArchives(Vulkan::Rasterizer& rasterizer,
                                         const ProgressFn& progress) {
    ScanSummary summary{};
    const auto archives = FindShaderBndArchives();
    summary.archives_found = static_cast<u32>(archives.size());
    if (archives.empty()) {
        std::printf("GPU: no .shaderbnd.dcx archives found for ahead-of-time precompile\n");
        return summary;
    }

    std::unordered_set<u64> unique_hashes;
    std::unordered_map<u64, ExtractedShader> extracted_map;
    std::vector<u64> extracted_order;

    for (u32 idx = 0; idx < summary.archives_found; ++idx) {
        const auto& path = archives[idx];
        if (progress) {
            progress("Extracting " + path.filename().string() + "...", idx,
                     summary.archives_found);
        }
        std::vector<u8> payload;
        if (!DecompressDcxDflt(path, payload)) {
            std::printf("GPU: warning: failed to decompress %s\n", path.string().c_str());
            continue;
        }
        ++summary.archives_ok;

        for (size_t at = 0; at + 8 <= payload.size(); ++at) {
            const auto prog = ParseOrbProgramAt(payload, at);
            if (!prog) {
                continue;
            }
            ++summary.total_orbshdr;
            const bool first_seen = unique_hashes.insert(prog->hash).second;
            switch (prog->type) {
            case 0: ++summary.ps_count; break;
            case 1: ++summary.vs_count; break;
            case 3: ++summary.cs_count; break;
            default: ++summary.gs_hs_ds_count; break;
            }
            if (first_seen && (prog->type == 0 || prog->type == 1 || prog->type == 3)) {
                ExtractedShader sh{};
                bool ok = false;
                if (prog->type == 3) {
                    ok = TryExtractCs(payload, *prog, sh);
                } else if (prog->type == 0) {
                    ok = TryExtractPs(payload, *prog, sh);
                } else if (prog->type == 1) {
                    ok = TryExtractVs(payload, *prog, sh);
                }
                if (ok && extracted_map.emplace(prog->hash, std::move(sh)).second) {
                    extracted_order.push_back(prog->hash);
                }
            }
            at = prog->footer + 27;
        }
        payload.clear();
        payload.shrink_to_fit();
        CompactHostHeaps();
    }
    summary.unique_hashes = static_cast<u32>(unique_hashes.size());
    std::printf("GPU: extracted %zu compilable CS/PS/VS programs from %u/%u .shaderbnd.dcx "
                "archives (%u OrbShdr total, %u unique)\n",
                extracted_order.size(), summary.archives_ok, summary.archives_found,
                summary.total_orbshdr, summary.unique_hashes);

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
    const u32 total = static_cast<u32>(extracted_order.size());

    // Compile CS first, then PS, then VS so compute & fragment NIR/DXIL land earliest.
    std::ranges::stable_sort(extracted_order, [&](u64 a, u64 b) {
        return static_cast<u8>(extracted_map[a].kind) < static_cast<u8>(extracted_map[b].kind);
    });

    for (u32 i = 0; i < total; ++i) {
        if ((i % 32u) == 0u && !HasSafeMemoryHeadroom()) {
            std::printf("GPU: stopping .shaderbnd.dcx precompile early at %u/%u to preserve memory headroom\n",
                        i, total);
            break;
        }
        auto it = extracted_map.find(extracted_order[i]);
        if (it == extracted_map.end()) {
            continue;
        }
        ++summary.precompiled_attempted;
        if (CompileSingleExtractedSafe(&ctx, &it->second)) {
            ++summary.precompiled_ok;
        } else {
            ++summary.precompiled_skipped;
        }
        // Release each shader's ISA buffer immediately as we go.
        it->second.code.clear();
        it->second.code.shrink_to_fit();

        if ((i % 8u) == 0u || i + 1 == total) {
            if (progress) {
                char label[192]{};
                std::snprintf(
                    label, sizeof(label),
                    "Precompiling Bloodborne shaders (%u / %u, DXIL cached: %u)...",
                    i + 1, total, summary.precompiled_ok);
                progress(label, i + 1, total);
            }
        }
        if ((i % 32u) == 31u) {
            CompactHostHeaps();
        }
        if ((i % 512u) == 511u) {
            pipe_cache.FlushCaches();
        }
    }

    extracted_map.clear();
    CompactHostHeaps();
    pipe_cache.FlushCaches();

    const size_t vkcache_bytes = pipe_cache.GetDriverCacheBytes();
    std::printf("GPU: .shaderbnd.dcx ahead-of-time DXIL precompile finished: "
                "%u cached / %u attempted (%u skipped), driver.vkcache = %.2f MiB\n",
                summary.precompiled_ok, summary.precompiled_attempted,
                summary.precompiled_skipped, double(vkcache_bytes) / (1024.0 * 1024.0));
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
    char summary[360]{};
    std::snprintf(summary, sizeof(summary),
                  "Precompiled: %u/%u archives (%u DXIL cached / %u unique) | "
                  "Warm-up: %u/%u PSOs | Cache: %.1f MiB (+%u in / +%u out)",
                  scan.archives_ok, scan.archives_found, scan.precompiled_ok, scan.unique_hashes,
                  warmed_ok, warmed_total, cache_mib, imported, exported);
    return summary;
}

} // namespace BbPrecompile
