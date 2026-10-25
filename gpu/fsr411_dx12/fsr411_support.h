// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <string>

struct Fsr411Support {
    bool supported;
    const char* reason;
    std::string detail{};
};
// Query the exact adapter/provider without creating an upscaler context or rendering.
Fsr411Support ProbeFsr411Dx12(const uint8_t luid[8]);
