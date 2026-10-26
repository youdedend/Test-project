// bbport: GPU-side assertion failures stop the port with exit code 23.
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "common/assert.h"
#include "common/logging/log.h"
#ifdef BB_UWP
#include <windows.h>
#endif

namespace BbPrecompile {
thread_local bool g_soft_assert_scope = false;
} // namespace BbPrecompile

void assert_fail_impl() {
    if (BbPrecompile::g_soft_assert_scope) {
        throw std::runtime_error("shader precompile assertion");
    }
    std::fflush(stdout);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
#ifdef BB_UWP
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 23);
#endif
    std::_Exit(23);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
