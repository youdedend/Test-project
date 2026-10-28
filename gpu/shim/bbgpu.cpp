#include "bbport_write_log.h"
// bbport: glue between the C loader and the vendored shadPS4 video core.
#include "bb_precompile.h"
#include "bbport_overlay.h"
#include "bbport_settings.h"
#include "bbport_copy.h"
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include "bbport_toggles.h"
#include <algorithm>
#include <cstdio>
#ifdef _WIN32
#include <windows.h> // GetCurrentThreadId for the per-thread operation table
#endif
#include <atomic>
#include <condition_variable>
#include <boost/asio/io_context.hpp>
#include "common/polyfill_thread.h"
#include "video_core/renderdoc.h"
#include <cstring>
#include <mutex>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#include "../bbgpu.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/rdtsc.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "core/memory.h"
#include "core/signals.h"
#include "sdl_window.h"
#include "bloodborne_cam.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
// bbport: the memory categories read the instance's accounting and the scheduler's fence position,
// so their definitions (not just the handles from vk_rasterizer.h) are needed here.
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

extern "C" {
// runtime_memory.c
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_write_backing(uintptr_t address, const void* data, uint64_t size);
void runtime_memory_read_backing(uintptr_t address, void* data, uint64_t size);
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
typedef void (*RuntimeGpuRange)(uintptr_t address, uint64_t size);
// runtime_kernel.c: one clock for guest and GPU timestamps
uint64_t runtime_process_time_us(void);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);
int32_t* runtime_errno(void);
void runtime_memory_set_gpu_hooks(RuntimeGpuRange map, RuntimeGpuRange unmap, RuntimeGpuRange invalidate);
}

Frontend::WindowSDL* g_window = nullptr;

namespace Libraries::GnmDriver { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::AvPlayer { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::VideoOut { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::Kernel { void RegisterEventQueue(Core::Loader::SymbolsResolver* sym); }

namespace {
struct Symbol {
    std::string nid, library, module;
    u64 address;
};
std::vector<Symbol> g_symbols;
u32 g_sdk_version;
} // namespace

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* nid, const char* library, const char* module, SymbolType,
                                u64 address) {
    g_symbols.push_back({nid, library, module, address});
}
} // namespace Core::Loader

// ElfInfo's fields are private to Core::Emulator; the port fills them here.
namespace Core {
class Emulator {
public:
    static void FillElfInfo(const BbGpuConfig& config) {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = config.serial ? config.serial : "UNKNOWN";
        info.title = config.title ? config.title : "";
        info.sdk_ver = config.sdk_version;
        info.psf_attributes.raw = config.psf_attributes;
    }
};

void MemoryManager::SetRasterizer(Vulkan::Rasterizer* rasterizer_) {
    rasterizer = rasterizer_;
    // Existing guest mappings are replayed by the runtime when hooks are installed.
    runtime_memory_set_gpu_hooks(
        [](uintptr_t address, uint64_t size) {
            auto* r = Memory::Instance()->GetRasterizer();
            r->RegisterMemory(address, size);
            r->MapMemory(address, size);
        },
        [](uintptr_t address, uint64_t size) { Memory::Instance()->GetRasterizer()->UnmapMemory(address, size); },
        [](uintptr_t address, uint64_t size) {
            Memory::Instance()->GetRasterizer()->InvalidateMemory(address, size);
        });
}
void MemoryManager::InvalidateMemory(VAddr address, u64 size) {
    if (rasterizer) rasterizer->InvalidateMemory(address, size);
}
bool MemoryManager::IsValidMapping(VAddr virtual_addr, u64 size) {
    return runtime_memory_is_mapped(virtual_addr, size) != 0;
}
u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    return runtime_memory_clamp(virtual_addr, size);
}
static void CopySparseSerial(VAddr source, u8* dest, u64 size) {
    // BB_READBACKS=2 protects GPU-written pages against reads: the copy threads and recorders
    // read through the backing view, as a read fault on them would wait for the GPU thread,
    // which waits for them.
    static const bool precise =
        EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Precise;
    if (precise) {
        runtime_memory_read_backing(source, dest, size);
        return;
    }
    while (size) {
        uintptr_t start = 0, end = 0;
        int mapped = 0;
        if (!runtime_memory_region(source, &start, &end, &mapped)) {
            end = source + size;
        }
        const u64 n = std::min<u64>(size, end - source);
        if (mapped) std::memcpy(dest, reinterpret_cast<const void*>(source), n);
        else std::memset(dest, 0, n);
        source += n; dest += n; size -= n;
    }
}
void MemoryManager::CopySparseMemory(VAddr source, u8* dest, u64 size) {
    // bbport: large uploads (streaming) are split across the copy threads.
    constexpr u64 Chunk = 512 * 1024;
    if (size < 4 * Chunk || !BbCopy::Enabled()) {
        return CopySparseSerial(source, dest, size);
    }
    BbCopy::ParallelFor((size + Chunk - 1) / Chunk, [&](std::size_t i) {
        const u64 offset = i * Chunk;
        CopySparseSerial(source + offset, dest + offset, std::min(Chunk, size - offset));
    });
}
bool MemoryManager::TryWriteBacking(void* address, const void* data, u64 size) {
    BbWriteLog::Note(reinterpret_cast<uintptr_t>(address), data, size, BbWriteLog::Backing);
    return runtime_memory_write_backing(reinterpret_cast<uintptr_t>(address), data, size) != 0;
}
void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    BbStats::Timer timer{BbStats::t_protect};
    const u64 pages = (size + 4095) / 4096;
    BbStats::protect_calls.fetch_add(1, std::memory_order_relaxed);
    BbStats::protect_pages.fetch_add(pages, std::memory_order_relaxed);
    if (!True(perms & MemoryPermission::Write)) {
        BbStats::protect_revoke_calls.fetch_add(1, std::memory_order_relaxed);
        BbStats::protect_revoke_pages.fetch_add(pages, std::memory_order_relaxed);
    }
    runtime_memory_gpu_protect(virtual_addr, size, True(perms & MemoryPermission::Read),
                               True(perms & MemoryPermission::Write));
}
boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions() {
    boost::icl::interval_set<VAddr> set;
    set += boost::icl::interval<VAddr>::right_open(0x10'0000'0000ULL, 0xfc'0000'0000ULL);
    return set;
}
} // namespace Core

// Kernel services the vendored libraries call directly.
namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetTscFrequency() { return runtime_tsc_frequency(); }
u64 PS4_SYSV_ABI sceKernelReadTsc() { return Common::FencedRDTSC(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTime() { return runtime_process_time_us(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() { return runtime_process_time_counter(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() { return 1'000'000'000; }
s32 PS4_SYSV_ABI sceKernelIsNeoMode() { return 0; } // base PS4
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver) {
    if (!ver) return ORBIS_KERNEL_ERROR_EINVAL;
    *ver = s32(g_sdk_version);
    return ORBIS_OK;
}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return ORBIS_OK;
}
} // namespace Libraries::Kernel

namespace {
// SDL video must be driven from one thread: the window lives on its own host thread.
std::thread g_window_thread;
std::mutex g_window_mutex;
std::condition_variable g_window_cv;
bool g_window_ready;
} // namespace

u32 BbDisplayRefreshHz() {
    static const u32 hz = [] {
#ifdef BB_UWP
        const char* env = std::getenv("BB_DISPLAY_HZ");
        const u32 rate = env && *env ? u32(std::strtoul(env, nullptr, 10)) : 60u;
        std::printf("GPU: vblank follows the display refresh rate, %u Hz\n", rate ? rate : 60u);
        return std::max<u32>(rate, 60u);
#else
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
        const u32 rate = mode && mode->refresh_rate > 0 ? u32(mode->refresh_rate + 0.5f) : 60;
        std::printf("GPU: vblank follows the display refresh rate, %u Hz\n", rate);
        return std::max<u32>(rate, 60);
#endif
    }();
    return hz;
}

#ifdef BB_PGO_GENERATE
extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);
// Instrumented build (BB_PGO=generate): the game often ends through _exit (watchdog, guest
// exit), which skips the profile write at exit. Write every 30 s and reset: the files sum the
// intervals.
static void StartProfileWriter() {
    std::thread([] {
        Common::SetCurrentThreadName("bb:pgo");
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            __gcov_dump();
            __gcov_reset();
            std::printf("PGO: profile written\n");
        }
    }).detach();
}
#endif

extern "C" int bbgpu_init(const BbGpuConfig* config) {
    BbSettings::Load();
#ifdef BB_PGO_GENERATE
    StartProfileWriter();
#endif
    g_sdk_version = config->sdk_version;
#ifdef _WIN32
    if (config->user_dir && !std::getenv("BB_GPU_USER_DIR")) _putenv_s("BB_GPU_USER_DIR", config->user_dir);
#else
    if (config->user_dir) setenv("BB_GPU_USER_DIR", config->user_dir, 0);
#endif
    Core::Emulator::FillElfInfo(*config);
#ifdef BB_UWP
    BbPrecompile::SyncPortableCacheToLocal();
#endif
    const std::string title = config->title ? config->title : "Bloodborne";
    const s32 width = config->width, height = config->height;
    g_window_thread = std::thread([title, width, height] {
        Common::SetCurrentThreadName("bb:window");
        auto* window = new Frontend::WindowSDL(width, height, title.c_str());
        {
            std::scoped_lock lock{g_window_mutex};
            g_window = window;
            g_window_ready = true;
        }
        g_window_cv.notify_all();
        while (window->PollEvents()) {
#ifndef BB_UWP
            SDL_Delay(2);
#endif
        }
        LOG_INFO(Frontend, "Window closed by user");
        std::fflush(stdout);
        std::_Exit(0);
    });
    g_window_thread.detach();
    {
        std::unique_lock lock{g_window_mutex};
        g_window_cv.wait(lock, [] { return g_window_ready; });
    }
    Core::Loader::SymbolsResolver resolver;
    // GnmDriver creates the presenter that the VideoOut present thread uses.
    Libraries::GnmDriver::RegisterLib(&resolver);
    Libraries::VideoOut::RegisterLib(&resolver);
    return 0;
}

namespace Libraries::Kernel { void StartKernelService(); }
extern "C" void bbnet_register(void); // bbnet/bbnet_glue.cpp: network and PSN (co-op)
extern "C" void bbgpu_register_kernel(void) {
    Libraries::Kernel::StartKernelService();
    Core::Loader::SymbolsResolver resolver;
    Libraries::Kernel::RegisterEventQueue(&resolver);
    Libraries::AvPlayer::RegisterLib(&resolver);
    bbnet_register();
}

extern "C" uintptr_t bbgpu_resolve(const char* scoped_nid) {
    const char* hash = std::strchr(scoped_nid, '#');
    const size_t length = hash ? size_t(hash - scoped_nid) : std::strlen(scoped_nid);
    for (const auto& symbol : g_symbols) {
        if (symbol.nid.size() == length && !std::memcmp(symbol.nid.data(), scoped_nid, length)) {
            return uintptr_t(symbol.address);
        }
    }
    return 0;
}

extern "C" int bbgpu_handle_fault(void* ucontext, void* address) {
    return Core::Signals::Instance()->DispatchAccessViolation(ucontext, address) ? 1 : 0;
}

extern "C" unsigned bbgpu_symbol_count(void) {
    return unsigned(g_symbols.size());
}

// Kernel service thread: runs boost::asio timers for equeue timer events.
namespace Libraries::Kernel {
boost::asio::io_context io_context;
static std::mutex m_asio_req;
static std::condition_variable_any cv_asio_req;
static std::atomic<u32> asio_requests;
static std::jthread service_thread;

void KernelSignalRequest() {
    std::unique_lock lock{m_asio_req};
    ++asio_requests;
    cv_asio_req.notify_one();
}

static void KernelServiceThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("bb:kernel_service");
    while (!stoken.stop_requested()) {
        {
            std::unique_lock lock{m_asio_req};
            cv_asio_req.wait(lock, stoken, [] { return asio_requests != 0; });
        }
        if (stoken.stop_requested()) break;
        io_context.run();
        io_context.restart();
        asio_requests = 0;
    }
}

void StartKernelService() {
    service_thread = std::jthread{KernelServiceThread};
}

int* PS4_SYSV_ABI __Error() {
    return runtime_errno();
}
} // namespace Libraries::Kernel

// RenderDoc capture hooks: not used by the port.
namespace VideoCore {
void LoadRenderDoc() {}
void StartCapture() {}
void EndCapture() {}
void TriggerCapture() {}
void SetOutputDir(const std::filesystem::path&, const std::string&) {}
bool IsRenderDocLoaded() { return false; }
void RequestScreenshot(ScreenshotRequest) {}
u32 ConsumeGameOnlyScreenshotRequests() { return 0; }
u32 ConsumeWithOverlaysScreenshotRequests() { return 0; }
ScreenshotRequests ConsumeScreenshotRequests() { return {}; }
} // namespace VideoCore

/// bbport: last-resort reclaim for a failed allocation (handoff §10). The buffer path asks for it
/// after the guest pages have been paged out; it returns what the texture cache could still free.
/// bbport: the last rung of the memory ladder, taken automatically at memory level 2 (the console's
/// own "critical" mark) and released when the pressure is gone for a while.
///
/// What it lowers is the *internal* render resolution, and only through a mechanism that already
/// exists and is reversible: the upscaler's preset, one step at a time (1080p -> 1.5x -> 1.7x of
/// the output). With no upscaler active the scene is the guest's own surfaces, so there is nothing
/// here to scale - `.1119` ran with the upscaler off, which is why this logs that fact once per
/// excursion instead of pretending to act.
///
/// Deliberately not done here: writing the setting to bbport.ini (the user's choice survives a
/// restart; this one does not) and touching the output resolution (a restart-level change).
/* bbport: the breadcrumb ring (xbox/platform/main.cpp). A resolution change is exactly the kind of
 * event a crash report cannot reconstruct afterwards. */
extern "C" void bb_uwp_breadcrumb(const char *text);

extern "C" void bbgpu_uwp_pressure_level(int level) {
    auto& s = BbSettings::Get();
    /* One line when the level changes, not one per sample: this is called every heartbeat. */
    {
        static int last_level = -1;
        if (level != last_level) {
            last_level = level;
            char line[96];
            std::snprintf(line, sizeof(line), "memory pressure level -> %d", level);
            bb_uwp_breadcrumb(line);
        }
    }
    static int saved_preset = -1;
    static int saved_output = -1;
    static std::chrono::steady_clock::time_point calm_since{};
    const int upscaler = s.upscaler.load(std::memory_order_relaxed);
    if (upscaler == BbSettings::UpscalerOff) {
        // No upscaler: the host renders the guest's surfaces at the output size, so the internal
        // resolution is the *output* one. 1280x720 there is a real saving in host targets (and the
        // text the GPU already prints recommends it) and it is a live change, unlike the startup
        // patch. Restored by the same ten-calm-seconds rule below.
        if (level >= 2) {
            calm_since = {};
            const int current = s.output_res.load(std::memory_order_relaxed);
            if (saved_output < 0 && current != 0) {
                saved_output = current;
                s.output_res.store(0, std::memory_order_relaxed);
                std::printf("UWP memory pressure: level %d, no upscaler - output %dx%d -> %dx%d "
                            "(live, restored when the pressure is gone)\n",
                            level, BbSettings::OutputWidths[saved_output],
                            BbSettings::OutputHeights[saved_output], BbSettings::OutputWidths[0],
                            BbSettings::OutputHeights[0]);
                std::fflush(stdout);
            }
            return;
        }
        if (saved_output < 0) return;
        const auto calm = std::chrono::steady_clock::now();
        if (calm_since.time_since_epoch().count() == 0) {
            calm_since = calm;
            return;
        }
        if (calm - calm_since < std::chrono::seconds(10)) return;
        std::printf("UWP memory pressure: level %d again, output restored to %dx%d\n", level,
                    BbSettings::OutputWidths[saved_output], BbSettings::OutputHeights[saved_output]);
        s.output_res.store(saved_output, std::memory_order_relaxed);
        saved_output = -1;
        calm_since = {};
        std::fflush(stdout);
        return;
    }
    if (level >= 2) {
        calm_since = {};
        if (saved_preset < 0) saved_preset = s.preset.load(std::memory_order_relaxed);
        const int preset = s.preset.load(std::memory_order_relaxed);
        // One step per sample, and no further than Performance (1.7x): past that the guest is
        // rendering 720p-class pixels and the saving is small next to the picture.
        if (preset < BbSettings::Performance) {
            const int next = preset + 1;
            s.preset.store(next, std::memory_order_relaxed);
            std::printf("UWP memory pressure: level %d, internal render resolution %s -> %s "
                        "(upscaler %s)\n",
                        level, BbSettings::PresetName(preset), BbSettings::PresetName(next),
                        BbSettings::UpscalerName(upscaler));
            std::fflush(stdout);
        }
        return;
    }
    if (saved_preset < 0 || saved_preset == s.preset.load(std::memory_order_relaxed)) {
        saved_preset = -1;
        return;
    }
    // Restore only after ten calm seconds: paging out of an area load dips the level for a moment,
    // and flipping the resolution back mid-load costs more than it saves.
    const auto now = std::chrono::steady_clock::now();
    if (calm_since.time_since_epoch().count() == 0) {
        calm_since = now;
        return;
    }
    if (now - calm_since < std::chrono::seconds(10)) return;
    std::printf("UWP memory pressure: level %d again, internal render resolution restored to %s\n",
                level, BbSettings::PresetName(saved_preset));
    s.preset.store(saved_preset, std::memory_order_relaxed);
    saved_preset = -1;
    calm_since = {};
    std::fflush(stdout);
}

extern "C" uint64_t bbgpu_uwp_emergency_reclaim(uint64_t need) {
    auto* r = Core::Memory::Instance()->GetRasterizer();
    if (!r) {
        return 0;
    }
    // bbport: B9's "retired-arena release" half, which until now only ever happened from
    // Rasterizer::OnSubmit. `.819` sat at `arena: live=912 retired=448 MiB` while the app was at its
    // ceiling and the texture cache was being emptied - 448 MiB of replaced arenas waiting for a
    // release that the submit path defers whenever a draw-prep producer is in flight (which, at
    // world load, is always).
    //
    // ReleaseRetiredArenas only frees an arena whose replacement tick the GPU has already signalled
    // and which no DMA view still hands to a descriptor set, so this cannot free memory that is in
    // use; it just asks again, now, because an allocation is waiting.
    uint64_t freed = 0;
    auto& buffers = r->GetBufferCache();
    const uint64_t retired_before = buffers.RetiredArenaBytes();
    if (retired_before != 0 && need != 0) {
        buffers.ReleaseRetiredArenas();
        const uint64_t retired_after = buffers.RetiredArenaBytes();
        if (retired_after < retired_before) {
            freed = retired_before - retired_after;
            std::printf("UWP emergency reclaim: released %llu MiB of retired dense arenas "
                        "(%llu MiB still retired)\n",
                        static_cast<unsigned long long>(freed >> 20),
                        static_cast<unsigned long long>(retired_after >> 20));
            std::fflush(stdout);
            bb_uwp_breadcrumb("emergency reclaim: retired arenas released");
        }
    }
    // `need - freed`, never 0: EmergencyReclaim's argument is a byte budget, and asking it for zero
    // bytes is not a way of asking for nothing.
    if (need > freed) {
        freed += r->GetTextureCache().EmergencyReclaim(need - freed);
    }
    return freed;
}

/// bbport: the memory categories and the fence position, for the UWP heartbeat (main.cpp).
///
/// The `.524` run showed why one number is not enough: `pageable private` fell to 508 MiB while the
/// app was still at 5,032/5,120 MiB, and the log could not say where the other 4.5 GiB was. Each
/// category here is a counter that already existed inside the renderer; this is the arithmetic that
/// puts them next to each other, and `unaccounted` in main.cpp is what is left.
extern "C" int32_t bbgpu_uwp_memory_cats(BbGpuMemoryCats *out) {
    if (!out) {
        return 0;
    }
    *out = BbGpuMemoryCats{};
    auto *r = Core::Memory::Instance()->GetRasterizer();
    if (!r) {
        return 0;
    }
    const auto &instance = r->GetInstance();
    if (instance.CanReportMemoryUsage()) {
        out->device_used = instance.GetDeviceMemoryUsage();
    }
    const auto &textures = r->GetTextureCache();
    out->texture_bytes = textures.ImageBytes();
    out->texture_count = textures.ImageCount();
    const auto &buffers = r->GetBufferCache();
    out->dense_live = buffers.ArenaBackedBytes();
    out->dense_retired = buffers.RetiredArenaBytes();
    out->arena_reserve_used = buffers.ArenaReserveUsedBytes();
    auto &scheduler = r->GetScheduler();
    out->fence_submitted = scheduler.CurrentTick();
    out->fence_completed = scheduler.GetWorkSemaphore()->KnownGpuTick();
    // bbport (B5): a fence age, without asking the scheduler for tick timestamps it does not keep.
    // "Completed has not moved while submitted is ahead of it" is the stall this is for; the clock
    // is the same one the heartbeat runs on, so the two lines can be read together.
    {
        static uint64_t last_completed = 0, last_change_ms = 0;
        const uint64_t now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        if (out->fence_completed != last_completed || last_change_ms == 0) {
            last_completed = out->fence_completed;
            last_change_ms = now_ms;
        }
        out->fence_stalled_ms = out->fence_submitted > out->fence_completed && now_ms > last_change_ms
                                    ? now_ms - last_change_ms
                                    : 0;
    }
    out->available = 1;
    return 1;
}

/// bbport: B8 - the big allocations and the slow ones, in one line each.
///
/// Two things go wrong at world load and neither used to leave a trace: a single allocation of
/// hundreds of MiB (which is what turns a run that was fine into the run that died), and an
/// allocation or a fence wait that takes a quarter of a second (the stall that looks like a hang in
/// the log but is really one call). Everything below this size is left alone - a log that prints
/// every 4 KiB page is not cheap enough for a release build.
///
/// `caller` is a static string literal; `elapsed_ms` is what the caller measured (0 when it does not
/// care). The usage numbers are the console's own, so the line says where the budget went.
extern "C" uint64_t bb_uwp_app_memory_usage();

namespace {
constexpr uint64_t kBigAllocBytes = UINT64_C(32) << 20;
constexpr double kSlowAllocMs = 250.0;
} // namespace

extern "C" void bbgpu_note_alloc(uint64_t size, const char *kind, const char *caller,
                                 uint64_t usage_before, double elapsed_ms) {
    const bool big = size >= kBigAllocBytes;
    const bool slow = elapsed_ms >= kSlowAllocMs;
    if (!big && !slow) {
        return;
    }
    std::printf("UWP alloc%s: %llu MiB %s (%s), usage %llu -> %llu MiB, %.1f ms%s\n",
                big ? "" : " (small but slow)", static_cast<unsigned long long>(size >> 20),
                kind ? kind : "unknown", caller ? caller : "unknown",
                static_cast<unsigned long long>(usage_before >> 20),
                static_cast<unsigned long long>(bb_uwp_app_memory_usage() >> 20), elapsed_ms,
                slow ? " [SLOW >250 ms]" : "");
    std::fflush(stdout);
}

/// bbport: per-thread "current operation", so a stall report says what each GPU thread was doing
/// rather than only where its stack was. `tag` is a string literal kept as a pointer, which is what
/// makes this safe to read from another thread: no buffer is ever rewritten, only swapped.
namespace {
struct ThreadOp {
    std::atomic<uint32_t> tid{0};
    std::atomic<const char *> tag{nullptr};
    std::atomic<uint64_t> arg0{0};
    std::atomic<uint64_t> arg1{0};
    std::atomic<uint64_t> since{0};
};
constexpr size_t kThreadOpSlots = 16;
ThreadOp g_thread_ops[kThreadOpSlots];
} // namespace

/* bbport: the slot is found once per thread and then cached, because `Scheduler::Wait` tags on
 * every fence wait - a 16-slot scan there would be paid thousands of times a second. Slots live in
 * a static array, so a cached pointer never dangles. */
static thread_local ThreadOp *t_slot = nullptr;

extern "C" void bbgpu_thread_op(const char *tag, uint64_t arg0, uint64_t arg1) {
    ThreadOp *slot = t_slot;
    if (!slot) {
        const uint32_t tid = static_cast<uint32_t>(::GetCurrentThreadId());
        ThreadOp *empty = nullptr;
        for (auto &op : g_thread_ops) {
            const uint32_t owner = op.tid.load(std::memory_order_relaxed);
            if (owner == tid) {
                slot = &op;
                break;
            }
            if (owner == 0 && !empty) {
                empty = &op;
            }
        }
        if (!slot) {
            slot = empty;
            if (!slot) {
                return; // more than 16 tagged threads: keep the ones already there
            }
            slot->tid.store(tid, std::memory_order_relaxed);
        }
        t_slot = slot;
    }
    slot->arg0.store(arg0, std::memory_order_relaxed);
    slot->arg1.store(arg1, std::memory_order_relaxed);
    slot->since.store(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::steady_clock::now().time_since_epoch())
                                                .count()),
                      std::memory_order_relaxed);
    slot->tag.store(tag, std::memory_order_release);
}

extern "C" void bbgpu_thread_op_done(void) {
    const uint32_t tid = static_cast<uint32_t>(::GetCurrentThreadId());
    for (auto &op : g_thread_ops) {
        if (op.tid.load(std::memory_order_relaxed) == tid) {
            op.tag.store(nullptr, std::memory_order_release);
            return;
        }
    }
}

extern "C" void bbgpu_thread_op_dump(void) {
    const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   std::chrono::steady_clock::now().time_since_epoch())
                                                   .count());
    for (auto &op : g_thread_ops) {
        const char *tag = op.tag.load(std::memory_order_acquire);
        if (!tag) {
            continue;
        }
        std::printf("UWP thread op: tid %u in '%s' (0x%llx, 0x%llx) for %llu ms\n",
                    op.tid.load(std::memory_order_relaxed), tag,
                    static_cast<unsigned long long>(op.arg0.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(op.arg1.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(now - op.since.load(std::memory_order_relaxed)));
    }
    std::fflush(stdout);
}

extern "C" void bbgpu_flush_caches(void) {
    if (auto* r = Core::Memory::Instance()->GetRasterizer()) {
        r->GetPipelineCache().FlushCaches();
    } else {
        Storage::DataBase::Instance().Flush();
    }
    BbPrecompile::ExportLocalCacheToPortable();
}

extern "C" int bbgpu_overlay_captures_input(void) {
    return BbOverlay::CapturesInput() ? 1 : 0;
}

extern "C" int bbgpu_audio_muted(void) {
    return g_window && BbSettings::Get().mute_unfocused && !g_window->IsFocused() ? 1 : 0;
}

extern "C" int bbgpu_text_input_begin(const char* initial, const char* prompt) {
    if (!g_window) return 0;
    g_window->BeginTextInput(initial ? initial : "", prompt ? prompt : "Text");
    return 1;
}

extern "C" int bbgpu_text_input_poll(char* out, uint64_t size) {
    if (!g_window) return 2;
    std::string text;
    const int state = g_window->PollTextInput(text);
    if (size) {
        const size_t n = std::min<size_t>(text.size(), size - 1);
        std::memcpy(out, text.data(), n);
        out[n] = 0;
    }
    return state;
}

extern "C" int bbgpu_choice_begin(const char* title, const char* const* items, int count, int focus) {
    if (!g_window || count <= 0 || !items) return 0;
    std::vector<std::string> labels;
    labels.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        labels.emplace_back(items[i] ? items[i] : "");
    }
    BbOverlay::BeginChoice(title ? title : "", labels, {}, focus);
    return 1;
}

extern "C" int bbgpu_choice_poll(void) {
    return BbOverlay::PollChoice();
}

extern "C" void bbgpu_mouse_take(BbMouseInput* out) {
    if (!out) return;
    if (!g_window) { std::memset(out, 0, sizeof(*out)); return; }
    Frontend::MouseInput mouse;
    g_window->TakeMouseInput(mouse);
    out->dx = mouse.dx;
    out->dy = mouse.dy;
    out->buttons = mouse.buttons;
    out->wheel = mouse.wheel;
    out->captured = mouse.captured ? 1 : 0;
    const Core::BloodborneCam& cam = Core::BloodborneCam::Instance();
    out->locked_on = cam.IsEnabled() && cam.IsLockedOn() ? 1 : 0;
}

extern "C" void bbgpu_input_configure(int mouse_mode_available, int32_t toggle_scancode, int32_t reload_scancode) {
    if (!g_window) return;
    g_window->ConfigureInput(mouse_mode_available != 0, toggle_scancode, reload_scancode);
}

extern "C" int bbgpu_input_reload_requested(void) {
    if (!g_window) return 0;
    return g_window->TakeInputReloadRequested();
}

extern "C" void bbgpu_mouse_camera_configure(int direct, float sensitivity, float scale_x, float scale_y, int sync) {
    Core::BloodborneCam::Instance().Configure(direct != 0, sensitivity, scale_x, scale_y, sync != 0);
}

#ifdef BB_UWP
extern "C" int bbgpu_uwp_active_precompile_diag(uint32_t* out_idx, uint32_t* out_total,
                                                uint32_t* out_stage, uint64_t* out_hash,
                                                uint64_t* out_partner_hash) {
    const auto d = BbPrecompile::GetActiveShaderDiagnostic();
    if (!d.active) {
        return 0;
    }
    if (out_idx) *out_idx = d.index;
    if (out_total) *out_total = d.total;
    if (out_stage) *out_stage = d.stage;
    if (out_hash) *out_hash = d.hash;
    if (out_partner_hash) *out_partner_hash = d.partner_hash;
    return 1;
}
#endif

