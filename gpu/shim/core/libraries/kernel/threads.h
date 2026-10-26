// bbport: host threads that may call guest code (AvPlayer allocator callbacks).
// Each thread gets a guest TCB (GS base, TLS) from the C runtime before running.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <stop_token>
#include <system_error>
#include <thread>
#include "common/types.h"
#include "core/libraries/kernel/threads/pthread.h"

extern "C" void runtime_thread_attach_host(const char* name);
#ifdef BB_UWP
extern "C" void* runtime_low_stack_alloc(size_t size);
extern "C" void runtime_low_stack_free(void* ptr, size_t size);
extern "C" void* compat_call_on_low_stack(void* (*fn)(void*), void* arg, void* stack_base,
                                          size_t stack_size);
#endif

namespace Libraries::Kernel {
class Thread {
public:
    Thread() = default;
    ~Thread() { Stop(); }
    void Run(std::function<void(std::stop_token)>&& func) {
        finished = std::make_shared<std::atomic<bool>>(false);
        thread = std::jthread([func = std::move(func), done = finished](std::stop_token stop) {
            runtime_thread_attach_host("bb:hle");
#ifdef BB_UWP
            constexpr size_t kLowStackBytes = 2 * 1024 * 1024;
            if (void* low_stack = runtime_low_stack_alloc(kLowStackBytes)) {
                struct CallCtx {
                    const std::function<void(std::stop_token)>* fn;
                    std::stop_token* st;
                } ctx{&func, &stop};
                auto tramp = [](void* p) -> void* {
                    auto* c = static_cast<CallCtx*>(p);
                    (*c->fn)(*c->st);
                    return nullptr;
                };
                compat_call_on_low_stack(+tramp, &ctx, low_stack, kLowStackBytes);
                runtime_low_stack_free(low_stack, kLowStackBytes);
            } else {
                func(stop);
            }
#else
            func(stop);
#endif
            done->store(true);
        });
    }
    // A thread may stop its own Thread object (AvPlayer does); it detaches instead of joining.
    // bbport: these are called from guest code, which a C++ exception cannot unwind: a failed
    // join is logged and the thread detached instead.
    void Join() {
        if (!thread.joinable()) return;
        try {
            if (thread.get_id() == std::this_thread::get_id()) thread.detach();
            else thread.join();
        } catch (const std::system_error& error) {
            // Windows (libc++ on Win32 threads): the join can fail although the thread is fine.
            // Wait for its function to return, so nothing it uses is freed under it, then let go.
            std::fprintf(stderr, "HLE thread: join failed (%s); waiting for it to finish\n", error.what());
            if (thread.get_id() != std::this_thread::get_id()) {
                for (int waited = 0; finished && !finished->load() && waited < 5000; ++waited) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            if (thread.joinable()) thread.detach();
        }
    }
    bool Joinable() const { return thread.joinable(); }
    void Stop() {
        if (thread.joinable()) {
            thread.request_stop();
            Join();
        }
    }

private:
    std::jthread thread;
    std::shared_ptr<std::atomic<bool>> finished;
};
} // namespace Libraries::Kernel
