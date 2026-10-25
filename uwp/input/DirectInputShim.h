// SPDX-License-Identifier: GPL-2.0-or-later
// DirectInput-to-Windows.Gaming.Input Remapping Shim & SDL2/SDL3 WinRT Xbox Mapping Verifier.
// DirectInput (dinput8.dll) is completely absent on Xbox UWP Dev Mode; this module translates
// legacy DIJOYSTATE2 packets, POV hats, and input.ini button indices into HostGamepadReading.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "../BBPort.UWP/Platform/GamepadInput.h"

namespace bbport::uwp::input {

// Binary-compatible subset of DirectInput8's DIJOYSTATE2 for legacy controller config shimming.
struct LegacyDiJoyState2 {
    int32_t lX = 32768;   // [0..65535], center = 32768
    int32_t lY = 32768;   // [0..65535], 0 = up, 65535 = down
    int32_t lZ = 0;       // combined or left trigger [0..65535]
    int32_t lRx = 32768;  // right stick X [0..65535]
    int32_t lRy = 32768;  // right stick Y [0..65535]
    int32_t lRz = 0;      // right trigger [0..65535]
    int32_t rglSlider[2] = {0, 0};
    uint32_t rgdwPOV[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu}; // hundredths of deg
    uint8_t rgbButtons[128]{};
};

// Translates a legacy DirectInput DualShock4 / Xbox HID state into a normalized HostGamepadReading
// suitable for Windows.Gaming.Input / Bloodborne OrbisPad consumption.
HostGamepadReading TranslateDirectInputState(const LegacyDiJoyState2& di, bool ds4_button_order = true);

// Maps legacy DirectInput / shadPS4 input.ini token names ("dinput_btn0", "pov_up", "back", etc.)
// to HostGamepadButtons bits so imported PC input.ini files work without dinput8.dll.
uint32_t ResolveInputIniTokenToHostButton(std::string_view token);

struct SdlWinrtAuditReport {
    bool guid_present = false;
    bool transform_attack_ok = false; // R1 -> L1 trick weapon transform attack
    bool item_use_ok = false;         // X -> Square quick item use
    bool visceral_attack_ok = false;  // LT -> L2 parry + forward LS + RB -> R1 visceral
    bool blood_bullets_ok = false;    // DPadUp -> +5 Quicksilver Bullets
    bool gestures_and_effects_ok = false; // View -> Left touchpad (480,471), View+DPadRight -> Right (1440,471)
    std::string mapping_line;

    bool AllPassed() const {
        return guid_present && transform_attack_ok && item_use_ok &&
               visceral_attack_ok && blood_bullets_ok && gestures_and_effects_ok;
    }
};

// Verifies the bundled SDL2/SDL3 WinRT controller DB entry for Xbox Series / One controllers
// and runs an end-to-end synthetic combat chord verification against Bloodborne's action map.
SdlWinrtAuditReport VerifySdlWinrtXboxMapping();

} // namespace bbport::uwp::input
