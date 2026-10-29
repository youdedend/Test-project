// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the Windows decommit API the compressed pool uses. This is not an Xbox hardware test.
#include <cstdio>
#include <cstdlib>
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <memoryapi.h>
// The standalone clang-cl test is not linked through the UWP target; this import lives in the
// same WindowsApp library used by the real package rather than the desktop kernel32 import set.
#pragma comment(lib, "WindowsApp.lib")

static void Require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s (Win32 error %lu)\n", message, GetLastError());
        std::exit(1);
    }
}
int main() {
    constexpr SIZE_T page = 4096, reservation = 65536;
    auto* base = static_cast<unsigned char*>(
        VirtualAllocFromApp(nullptr, reservation, MEM_RESERVE, PAGE_NOACCESS));
    Require(base != nullptr, "reserve");
    Require(VirtualAllocFromApp(base, 2 * page, MEM_COMMIT, PAGE_READWRITE) == base, "commit");
    base[0] = 0x42;
    base[page] = 0x73;
    Require(VirtualFree(base, page, MEM_DECOMMIT) != 0, "VirtualFree MEM_DECOMMIT");
    MEMORY_BASIC_INFORMATION info{};
    Require(VirtualQuery(base, &info, sizeof(info)) != 0 && info.State == MEM_RESERVE,
            "decommitted page remains reserved");
    Require(VirtualQuery(base + page, &info, sizeof(info)) != 0 && info.State == MEM_COMMIT &&
                base[page] == 0x73,
            "neighbouring live blob remains committed and unchanged");
    Require(VirtualAllocFromApp(base, page, MEM_COMMIT, PAGE_READWRITE) == base,
            "recommit freed page");
    Require(base[0] == 0 && base[page] == 0x73, "recommitted page is zero; neighbour survived");
    Require(VirtualFree(base, 0, MEM_RELEASE) != 0, "release reservation");
    std::puts("Windows swap-pool decommit: all checks passed");
}
#else
int main() {
    std::puts("Windows swap-pool decommit: skipped on non-Windows host");
}
#endif
