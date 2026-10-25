// SPDX-License-Identifier: GPL-2.0-or-later
// UWP PLM (Process Lifetime Management) & Xbox Game Mode 5 GiB Memory-Pressure Monitor.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

namespace bbport::uwp {

enum class MemoryPressureTier : uint8_t {
    Low = 0,       // < 75% of AppMemoryUsageLimit
    Medium = 1,    // 75%..85% — evict cold TextureSetMemo entries
    High = 2,      // 85%..93% — evict TextureSetMemo + SceneProxies + unused upload heaps
    OverLimit = 3, // >= 93% — emergency flush before Xbox OS terminates process
};

struct MemoryBudgetSnapshot {
    uint64_t usage_bytes = 0;
    uint64_t limit_bytes = 5ull * 1024ull * 1024ull * 1024ull; // 5 GiB Xbox Dev Mode Game Mode default
    MemoryPressureTier tier = MemoryPressureTier::Low;
};

class LifecycleManager {
public:
    using SaveCheckpointCallback = std::function<void()>;
    using MemoryPressureCallback = std::function<void(MemoryPressureTier, const MemoryBudgetSnapshot&)>;

    static LifecycleManager& Instance();

    void Initialize();
    void Shutdown();

    void SetSaveCheckpointCallback(SaveCheckpointCallback cb);
    void SetMemoryPressureCallback(MemoryPressureCallback cb);

    // Invoked by PLM Suspending/Resuming events (and callable directly from unit tests).
    void HandleSuspending();
    void HandleResuming();

    // Polls Windows::System::MemoryManager::AppMemoryUsage / AppMemoryUsageLimit (or host override).
    MemoryBudgetSnapshot PollMemoryBudget();
    static MemoryPressureTier ClassifyUsage(uint64_t usage_bytes, uint64_t limit_bytes);

    bool IsSuspended() const { return suspended_.load(std::memory_order_acquire); }
    uint64_t SuspendCount() const { return suspend_count_.load(std::memory_order_relaxed); }

private:
    LifecycleManager() = default;

    mutable std::mutex cb_mutex_;
    SaveCheckpointCallback save_cb_;
    MemoryPressureCallback mem_cb_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> suspended_{false};
    std::atomic<uint64_t> suspend_count_{0};
};

} // namespace bbport::uwp
