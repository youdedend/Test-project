// SPDX-License-Identifier: GPL-2.0-or-later
#include "LifecycleManager.h"
#if __has_include("StorageLayer.h")
#include "StorageLayer.h"
#define BBPORT_HAS_STORAGE_LAYER 1
#endif
#include "../../../gpu/bbgpu.h"

#if defined(_WIN32)
#include <windows.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#endif

namespace bbport::uwp {

namespace {
#if defined(_WIN32)
winrt::event_token g_suspending_token{};
winrt::event_token g_resuming_token{};
winrt::event_token g_mem_level_token{};
#endif
} // namespace

LifecycleManager& LifecycleManager::Instance() {
    static LifecycleManager inst;
    return inst;
}

MemoryPressureTier LifecycleManager::ClassifyUsage(uint64_t usage_bytes, uint64_t limit_bytes) {
    if (limit_bytes == 0) return MemoryPressureTier::Low;
    const uint64_t pct = (usage_bytes * 100ull) / limit_bytes;
    if (pct >= 93) return MemoryPressureTier::OverLimit;
    if (pct >= 85) return MemoryPressureTier::High;
    if (pct >= 75) return MemoryPressureTier::Medium;
    return MemoryPressureTier::Low;
}

void LifecycleManager::Initialize() {
    bool expected = false;
    if (!initialized_.compare_exchange_strong(expected, true)) {
        return;
    }

#if defined(_WIN32)
    try {
        using winrt::Windows::ApplicationModel::Core::CoreApplication;
        using winrt::Windows::System::MemoryManager;

        g_suspending_token = CoreApplication::Suspending(
            [this](const auto&, const winrt::Windows::ApplicationModel::SuspendingEventArgs& args) {
                // Acquire deferral immediately so Xbox OS waits up to the PLM deadline while
                // we flush atomic saves, wait for the D3D12 GPU queue, and call IDXGIDevice3::Trim.
                auto deferral = args.SuspendingOperation().GetDeferral();
                HandleSuspending();
                deferral.Complete();
            });

        g_resuming_token = CoreApplication::Resuming(
            [this](const auto&, const auto&) {
                HandleResuming();
            });

        g_mem_level_token = MemoryManager::AppMemoryUsageLevelChanged(
            [this](const auto&, const auto&) {
                PollMemoryBudget();
            });
    } catch (...) {
    }
#endif
}

void LifecycleManager::Shutdown() {
    if (!initialized_.exchange(false)) return;
#if defined(_WIN32)
    try {
        using winrt::Windows::ApplicationModel::Core::CoreApplication;
        using winrt::Windows::System::MemoryManager;
        if (g_suspending_token.value != 0) CoreApplication::Suspending(g_suspending_token);
        if (g_resuming_token.value != 0) CoreApplication::Resuming(g_resuming_token);
        if (g_mem_level_token.value != 0) MemoryManager::AppMemoryUsageLevelChanged(g_mem_level_token);
    } catch (...) {
    }
#endif
}

void LifecycleManager::SetSaveCheckpointCallback(SaveCheckpointCallback cb) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    save_cb_ = std::move(cb);
}

void LifecycleManager::SetMemoryPressureCallback(MemoryPressureCallback cb) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    mem_cb_ = std::move(cb);
}

void LifecycleManager::HandleSuspending() {
    suspended_.store(true, std::memory_order_release);
    suspend_count_.fetch_add(1, std::memory_order_relaxed);

    SaveCheckpointCallback save_cb_copy;
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        save_cb_copy = save_cb_;
    }
    if (save_cb_copy) {
        save_cb_copy();
    }

#if defined(BBPORT_HAS_STORAGE_LAYER)
    // Ensure userdata0010 sound flag & clean .bbtmp state are finalized on disk.
    StorageLayer::Instance().PrepareSaveDirectoryAndSoundFlag();
#endif

#if defined(BBPORT_UWP) && defined(BB_GPU_UWP_ABI_READY)
    // Drain D3D12 command queues and call IDXGIDevice3::Trim() if running on D3D11 fallback.
    bbgpu_uwp_on_suspending();
#endif
}

void LifecycleManager::HandleResuming() {
#if defined(BBPORT_UWP) && defined(BB_GPU_UWP_ABI_READY)
    bbgpu_uwp_on_resuming();
#endif
    suspended_.store(false, std::memory_order_release);
}

MemoryBudgetSnapshot LifecycleManager::PollMemoryBudget() {
    MemoryBudgetSnapshot snap{};
#if defined(_WIN32)
    try {
        using winrt::Windows::System::MemoryManager;
        snap.usage_bytes = MemoryManager::AppMemoryUsage();
        snap.limit_bytes = MemoryManager::AppMemoryUsageLimit();
    } catch (...) {
    }
#endif
    snap.tier = ClassifyUsage(snap.usage_bytes, snap.limit_bytes);

    if (snap.tier != MemoryPressureTier::Low) {
        MemoryPressureCallback mem_cb_copy;
        {
            std::lock_guard<std::mutex> lock(cb_mutex_);
            mem_cb_copy = mem_cb_;
        }
        if (mem_cb_copy) {
            mem_cb_copy(snap.tier, snap);
        }
#if defined(BBPORT_UWP) && defined(BB_GPU_UWP_ABI_READY)
        if (snap.tier == MemoryPressureTier::High || snap.tier == MemoryPressureTier::OverLimit) {
            const uint64_t target_reclaim = (snap.tier == MemoryPressureTier::OverLimit)
                ? (512ull * 1024ull * 1024ull)
                : (192ull * 1024ull * 1024ull);
            bbgpu_uwp_trim_memory(target_reclaim);
        }
#endif
    }
    return snap;
}

} // namespace bbport::uwp
