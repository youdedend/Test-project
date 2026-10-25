#include "fg401_support.h"
#include <cassert>
#include <cstdio>
int main() {
    assert(IsRx9000(0x1002, "AMD Radeon RX 9070"));
    assert(IsRx9000(0x1002, "AMD Radeon RX 9070 XT"));
    assert(IsRx9000(0x1002, "AMD Radeon RX 9060 XT"));
    assert(!IsRx9000(0x1002, "AMD Radeon RX 7900 XTX"));
    assert(!IsRx9000(0x1002, "AMD Radeon RX 6900 XT"));
    assert(!IsRx9000(0x10de, "AMD Radeon RX 9070"));
    assert(!IsRx9000(0x8086, "Intel Arc B580"));
    assert(!IsRx9000(0x1002, "AMD Radeon RX 907"));
    assert(!IsRx9000(0x1002, "AMD Radeon RX 90700"));
    assert(!IsRx9000(0x1002, "AMD Radeon RX 9abc"));
    assert(Fg401SupportMessage({false,"files","test.dll"}).find("test.dll") != std::string::npos);
    assert(Fg401SupportMessage({false,"windows11"}).find("Windows 11") != std::string::npos);
    assert(Fg401SupportMessage({false,"check"}).find("check failed") != std::string::npos);
    std::puts("PASS RX 9000 allowlist and vendor/generation rejection");
}
