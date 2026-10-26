// SPDX-License-Identifier: GPL-2.0-or-later
#include "input.h"
#include <windows.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <algorithm>
#include <cmath>
#include <mutex>

using namespace winrt::Windows::Gaming::Input;

namespace {
constexpr uint32_t kBtnL3 = 0x2u;
constexpr uint32_t kBtnR3 = 0x4u;
constexpr uint32_t kBtnOptions = 0x8u;
constexpr uint32_t kBtnUp = 0x10u;
constexpr uint32_t kBtnRight = 0x20u;
constexpr uint32_t kBtnDown = 0x40u;
constexpr uint32_t kBtnLeft = 0x80u;
constexpr uint32_t kBtnL2 = 0x100u;
constexpr uint32_t kBtnR2 = 0x200u;
constexpr uint32_t kBtnL1 = 0x400u;
constexpr uint32_t kBtnR1 = 0x800u;
constexpr uint32_t kBtnTriangle = 0x1000u;
constexpr uint32_t kBtnCircle = 0x2000u;
constexpr uint32_t kBtnCross = 0x4000u;
constexpr uint32_t kBtnSquare = 0x8000u;
constexpr uint32_t kBtnTouchpad = 0x100000u;

std::mutex g_touch_mutex;
bool g_view_was_down = false;
bool g_view_combo_used = false;
uint64_t g_touch_click_until_ms = 0;

void EnsureApartment() {
    thread_local bool ready = false;
    if (!ready) {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (winrt::hresult_error const&) {
        }
        ready = true;
    }
}

uint8_t StickByte(double v) {
    const int n = static_cast<int>(std::lround((std::clamp(v, -1.0, 1.0) + 1.0) * 127.5));
    return static_cast<uint8_t>(std::clamp(n, 0, 255));
}

uint8_t TriggerByte(double v) {
    const int n = static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
    return static_cast<uint8_t>(std::clamp(n, 0, 255));
}
} // namespace

extern "C" int bbport_uwp_gamepad_poll(uint32_t* out_buttons, uint8_t out_axes[6],
                                       int* out_touch_side, uint8_t* out_connected_count) {
    EnsureApartment();
    if (out_buttons) *out_buttons = 0;
    if (out_axes) {
        out_axes[0] = out_axes[1] = out_axes[2] = out_axes[3] = 128;
        out_axes[4] = out_axes[5] = 0;
    }
    if (out_touch_side) *out_touch_side = -1;
    if (out_connected_count) *out_connected_count = 1;

    try {
        const auto pads = Gamepad::Gamepads();
        const uint32_t count = pads.Size();
        if (out_connected_count) {
            *out_connected_count = static_cast<uint8_t>(std::clamp<uint32_t>(count, 1u, 4u));
        }
        if (count == 0) return 0;

        const auto reading = pads.GetAt(0).GetCurrentReading();
        const auto b = reading.Buttons;

        uint32_t buttons = 0;
        if ((b & GamepadButtons::A) != GamepadButtons::None) buttons |= kBtnCross;
        if ((b & GamepadButtons::B) != GamepadButtons::None) buttons |= kBtnCircle;
        if ((b & GamepadButtons::X) != GamepadButtons::None) buttons |= kBtnSquare;
        if ((b & GamepadButtons::Y) != GamepadButtons::None) buttons |= kBtnTriangle;
        if ((b & GamepadButtons::LeftShoulder) != GamepadButtons::None) buttons |= kBtnL1;
        if ((b & GamepadButtons::RightShoulder) != GamepadButtons::None) buttons |= kBtnR1;
        if ((b & GamepadButtons::LeftThumbstick) != GamepadButtons::None) buttons |= kBtnL3;
        if ((b & GamepadButtons::RightThumbstick) != GamepadButtons::None) buttons |= kBtnR3;
        if ((b & GamepadButtons::Menu) != GamepadButtons::None) buttons |= kBtnOptions;
        if ((b & GamepadButtons::DPadUp) != GamepadButtons::None) buttons |= kBtnUp;
        if ((b & GamepadButtons::DPadDown) != GamepadButtons::None) buttons |= kBtnDown;

        const bool view_down = (b & GamepadButtons::View) != GamepadButtons::None;
        const bool dpad_left = (b & GamepadButtons::DPadLeft) != GamepadButtons::None;
        const bool dpad_right = (b & GamepadButtons::DPadRight) != GamepadButtons::None;

        // DualShock 4 Touchpad mapping on Xbox controller (matches desktop touchpad_combo):
        // - Hold View + D-Pad Left  -> Left Touchpad  (Gestures, side 0; suppresses D-Pad Left)
        // - Hold View + D-Pad Right -> Right Touchpad (Personal Effects, side 1; suppresses D-Pad Right)
        // - Tap & release View alone -> Right Touchpad (Personal Effects, side 1, 150 ms pulse)
        int touch_side = -1;
        {
            std::scoped_lock lock(g_touch_mutex);
            const uint64_t now_ms = GetTickCount64();
            if (view_down) {
                if (!g_view_was_down) g_view_combo_used = false;
                if (dpad_left || dpad_right) {
                    touch_side = dpad_right ? 1 : 0;
                    g_view_combo_used = true;
                }
            } else {
                if (g_view_was_down && !g_view_combo_used) {
                    g_touch_click_until_ms = now_ms + 150;
                }
                if (dpad_left) buttons |= kBtnLeft;
                if (dpad_right) buttons |= kBtnRight;
            }
            g_view_was_down = view_down;
            if (touch_side < 0 && now_ms < g_touch_click_until_ms) {
                touch_side = 1;
            }
        }
        if (touch_side >= 0) {
            buttons |= kBtnTouchpad;
        }

        const uint8_t l2 = TriggerByte(reading.LeftTrigger);
        const uint8_t r2 = TriggerByte(reading.RightTrigger);
        if (l2 > 30) buttons |= kBtnL2;
        if (r2 > 30) buttons |= kBtnR2;

        if (out_buttons) *out_buttons = buttons;
        if (out_touch_side) *out_touch_side = touch_side;
        if (out_axes) {
            out_axes[0] = StickByte(reading.LeftThumbstickX);
            out_axes[1] = StickByte(-reading.LeftThumbstickY);
            out_axes[2] = StickByte(reading.RightThumbstickX);
            out_axes[3] = StickByte(-reading.RightThumbstickY);
            out_axes[4] = l2;
            out_axes[5] = r2;
        }
        return 1;
    } catch (winrt::hresult_error const&) {
        return 0;
    }
}

extern "C" void bbport_uwp_gamepad_set_vibration(uint8_t large_motor, uint8_t small_motor) {
    EnsureApartment();
    try {
        const auto pads = Gamepad::Gamepads();
        if (pads.Size() == 0) return;
        GamepadVibration vib{};
        vib.LeftMotor = static_cast<double>(large_motor) / 255.0;
        vib.RightMotor = static_cast<double>(small_motor) / 255.0;
        pads.GetAt(0).Vibration(vib);
    } catch (winrt::hresult_error const&) {
    }
}

extern "C" const char* bbport_uwp_gamepad_name(void) {
    return "Xbox Wireless Controller (Windows.Gaming.Input)";
}
