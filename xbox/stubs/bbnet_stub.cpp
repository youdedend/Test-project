// SPDX-License-Identifier: GPL-2.0-or-later
// Xbox UWP offline network stub: all 180 libSceNet / libSceNp* / libSceHttp / libSceSsl
// imports resolve to src/runtime_services.c's offline HLE contracts without pulling in
// OpenSSL, Protobuf, wepoll or miniupnpc.
#include <cstdint>
#include <cstdio>

extern "C" void bbnet_configure(const char* app0, const char* serial, const char* title,
                                const char* version) {
    (void)app0;
    std::printf("Online: Xbox UWP offline profile (%s %s %s)\n",
                serial ? serial : "UNKNOWN",
                title ? title : "Bloodborne",
                version ? version : "");
}

extern "C" void bbnet_initialize(void) {}

extern "C" void bbnet_set_image(unsigned char* image, uint64_t size) {
    (void)image;
    (void)size;
}

extern "C" void bbnet_register(void) {}
