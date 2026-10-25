// SPDX-License-Identifier: GPL-2.0-or-later
#include "DirectInputShim.h"

#include <algorithm>
#include <cctype>

namespace bbport::uwp::input {

namespace {

double NormalizeUnsignedAxis16(int32_t raw) {
    const double clamped = std::clamp(static_cast<double>(raw), 0.0, 65535.0);
    return std::clamp((clamped - 32767.5) / 32767.5, -1.0, 1.0);
}

std::string LowerAscii(std::string_view sv) {
    std::string out(sv);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// Canonical SDL2/SDL3 WinRT Windows.Gaming.Input Xbox controller mapping string.
constexpr const char* kSdlWinrtXboxControllerDbLine =
    "030000005e040000ea02000000007801,Xbox Wireless Controller (UWP WGI),"
    "a:b0,b:b1,x:b2,y:b3,back:b6,guide:b10,start:b7,leftstick:b8,rightstick:b9,"
    "leftshoulder:b4,rightshoulder:b5,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
    "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,platform:WinRT,";

} // namespace

HostGamepadReading TranslateDirectInputState(const LegacyDiJoyState2& di, bool ds4_button_order) {
    HostGamepadReading out{};

    out.left_thumb_x = NormalizeUnsignedAxis16(di.lX);
    // DirectInput Y is top-down (0 = up, 65535 = down); WinRT is bottom-up (+1 = up).
    out.left_thumb_y = -NormalizeUnsignedAxis16(di.lY);
    out.right_thumb_x = NormalizeUnsignedAxis16(di.lRx);
    out.right_thumb_y = -NormalizeUnsignedAxis16(di.lRy);
    out.left_trigger = std::clamp(di.lZ / 65535.0, 0.0, 1.0);
    out.right_trigger = std::clamp(di.lRz / 65535.0, 0.0, 1.0);

    // POV hat in hundredths of degrees clockwise from North (0 = N, 9000 = E, 18000 = S, 27000 = W).
    const uint32_t pov = di.rgdwPOV[0];
    if (pov != 0xFFFFFFFFu && (pov & 0xFFFFu) != 0xFFFFu && pov < 36000u) {
        if (pov >= 31500u || pov <= 4500u)  out.buttons |= HOST_PAD_DPAD_UP;
        if (pov >= 4500u  && pov <= 13500u) out.buttons |= HOST_PAD_DPAD_RIGHT;
        if (pov >= 13500u && pov <= 22500u) out.buttons |= HOST_PAD_DPAD_DOWN;
        if (pov >= 22500u && pov <= 31500u) out.buttons |= HOST_PAD_DPAD_LEFT;
    }

    if (ds4_button_order) {
        // DualShock 4 DirectInput HID layout:
        // 0=Square, 1=Cross, 2=Circle, 3=Triangle, 4=L1, 5=R1, 6=L2(btn), 7=R2(btn),
        // 8=Share, 9=Options, 10=L3, 11=R3, 13=TouchpadClick
        if (di.rgbButtons[0] & 0x80u)  out.buttons |= HOST_PAD_X;
        if (di.rgbButtons[1] & 0x80u)  out.buttons |= HOST_PAD_A;
        if (di.rgbButtons[2] & 0x80u)  out.buttons |= HOST_PAD_B;
        if (di.rgbButtons[3] & 0x80u)  out.buttons |= HOST_PAD_Y;
        if (di.rgbButtons[4] & 0x80u)  out.buttons |= HOST_PAD_LEFT_SHOULDER;
        if (di.rgbButtons[5] & 0x80u)  out.buttons |= HOST_PAD_RIGHT_SHOULDER;
        if ((di.rgbButtons[6] & 0x80u) && out.left_trigger < 0.5)  out.left_trigger = 1.0;
        if ((di.rgbButtons[7] & 0x80u) && out.right_trigger < 0.5) out.right_trigger = 1.0;
        if (di.rgbButtons[8] & 0x80u)  out.buttons |= HOST_PAD_VIEW;
        if (di.rgbButtons[9] & 0x80u)  out.buttons |= HOST_PAD_MENU;
        if (di.rgbButtons[10] & 0x80u) out.buttons |= HOST_PAD_LEFT_THUMB;
        if (di.rgbButtons[11] & 0x80u) out.buttons |= HOST_PAD_RIGHT_THUMB;
        if (di.rgbButtons[13] & 0x80u) out.buttons |= HOST_PAD_VIEW;
    } else {
        // Xbox DirectInput HID layout:
        // 0=A, 1=B, 2=X, 3=Y, 4=LB, 5=RB, 6=View, 7=Menu, 8=LS, 9=RS
        if (di.rgbButtons[0] & 0x80u) out.buttons |= HOST_PAD_A;
        if (di.rgbButtons[1] & 0x80u) out.buttons |= HOST_PAD_B;
        if (di.rgbButtons[2] & 0x80u) out.buttons |= HOST_PAD_X;
        if (di.rgbButtons[3] & 0x80u) out.buttons |= HOST_PAD_Y;
        if (di.rgbButtons[4] & 0x80u) out.buttons |= HOST_PAD_LEFT_SHOULDER;
        if (di.rgbButtons[5] & 0x80u) out.buttons |= HOST_PAD_RIGHT_SHOULDER;
        if (di.rgbButtons[6] & 0x80u) out.buttons |= HOST_PAD_VIEW;
        if (di.rgbButtons[7] & 0x80u) out.buttons |= HOST_PAD_MENU;
        if (di.rgbButtons[8] & 0x80u) out.buttons |= HOST_PAD_LEFT_THUMB;
        if (di.rgbButtons[9] & 0x80u) out.buttons |= HOST_PAD_RIGHT_THUMB;
    }

    return out;
}

uint32_t ResolveInputIniTokenToHostButton(std::string_view token) {
    const std::string t = LowerAscii(token);
    if (t == "cross" || t == "a" || t == "dinput_btn1")         return HOST_PAD_A;
    if (t == "circle" || t == "b" || t == "dinput_btn2")        return HOST_PAD_B;
    if (t == "square" || t == "x" || t == "dinput_btn0")        return HOST_PAD_X;
    if (t == "triangle" || t == "y" || t == "dinput_btn3")      return HOST_PAD_Y;
    if (t == "l1" || t == "lb" || t == "dinput_btn4")           return HOST_PAD_LEFT_SHOULDER;
    if (t == "r1" || t == "rb" || t == "dinput_btn5")           return HOST_PAD_RIGHT_SHOULDER;
    if (t == "l3" || t == "ls" || t == "dinput_btn10")          return HOST_PAD_LEFT_THUMB;
    if (t == "r3" || t == "rs" || t == "dinput_btn11")          return HOST_PAD_RIGHT_THUMB;
    if (t == "options" || t == "start" || t == "dinput_btn9")   return HOST_PAD_MENU;
    if (t == "back" || t == "view" || t == "touchpad" || t == "dinput_btn8" || t == "dinput_btn13")
        return HOST_PAD_VIEW;
    if (t == "pad_up" || t == "pov_up")                         return HOST_PAD_DPAD_UP;
    if (t == "pad_down" || t == "pov_down")                     return HOST_PAD_DPAD_DOWN;
    if (t == "pad_left" || t == "pov_left")                     return HOST_PAD_DPAD_LEFT;
    if (t == "pad_right" || t == "pov_right")                   return HOST_PAD_DPAD_RIGHT;
    if (t == "l4" || t == "paddle1")                            return HOST_PAD_PADDLE1;
    if (t == "r4" || t == "paddle2")                            return HOST_PAD_PADDLE2;
    if (t == "l5" || t == "paddle3")                            return HOST_PAD_PADDLE3;
    if (t == "r5" || t == "paddle4")                            return HOST_PAD_PADDLE4;
    return HOST_PAD_NONE;
}

SdlWinrtAuditReport VerifySdlWinrtXboxMapping() {
    SdlWinrtAuditReport report{};
    report.mapping_line = kSdlWinrtXboxControllerDbLine;
    report.guid_present =
        report.mapping_line.find("platform:WinRT") != std::string::npos &&
        report.mapping_line.find("lefttrigger:a4") != std::string::npos &&
        report.mapping_line.find("righttrigger:a5") != std::string::npos;

    GamepadProfile profile{};

    // 1. Transform Attack (RB -> R1 light swing, then LB -> L1 trick transform attack).
    HostGamepadReading r1_step{};
    r1_step.buttons = HOST_PAD_RIGHT_SHOULDER;
    HostGamepadReading l1_step{};
    l1_step.buttons = HOST_PAD_LEFT_SHOULDER;
    const auto m_r1 = TranslateReadingToBloodborne(r1_step, profile);
    const auto m_l1 = TranslateReadingToBloodborne(l1_step, profile);
    report.transform_attack_ok =
        (m_r1.buttons == ORBIS_BTN_R1) && (m_l1.buttons == ORBIS_BTN_L1);

    // 2. Quick Item Use (Xbox X -> Orbis Square).
    HostGamepadReading item_step{};
    item_step.buttons = HOST_PAD_X;
    const auto m_item = TranslateReadingToBloodborne(item_step, profile);
    report.item_use_ok = (m_item.buttons == ORBIS_BTN_SQUARE);

    // 3. Visceral Attack (LT -> L2 firearm parry, then forward LS + RB -> R1 visceral riposte).
    HostGamepadReading parry_step{};
    parry_step.left_trigger = 0.95;
    HostGamepadReading riposte_step{};
    riposte_step.left_thumb_y = 1.0; // full forward -> Orbis ly == 0
    riposte_step.buttons = HOST_PAD_RIGHT_SHOULDER;
    const auto m_parry = TranslateReadingToBloodborne(parry_step, profile);
    const auto m_riposte = TranslateReadingToBloodborne(riposte_step, profile);
    report.visceral_attack_ok =
        ((m_parry.buttons & ORBIS_BTN_L2) != 0 && m_parry.axes[4] >= 240) &&
        ((m_riposte.buttons & ORBIS_BTN_R1) != 0 && m_riposte.axes[1] == 0);

    // 4. Blood Bullets (DPadUp -> +5 Quicksilver Bullets).
    HostGamepadReading bullets_step{};
    bullets_step.buttons = HOST_PAD_DPAD_UP;
    report.blood_bullets_ok =
        (TranslateReadingToBloodborne(bullets_step, profile).buttons == ORBIS_BTN_UP);

    // 5. Gestures (View -> Left Touchpad side 0) & Personal Effects (View + DPadRight -> side 1, DPadRight suppressed).
    HostGamepadReading gesture_step{};
    gesture_step.buttons = HOST_PAD_VIEW;
    HostGamepadReading effects_step{};
    effects_step.buttons = HOST_PAD_VIEW | HOST_PAD_DPAD_RIGHT;
    const auto m_gesture = TranslateReadingToBloodborne(gesture_step, profile);
    const auto m_effects = TranslateReadingToBloodborne(effects_step, profile);
    report.gestures_and_effects_ok =
        ((m_gesture.buttons & ORBIS_BTN_TOUCHPAD) != 0 && m_gesture.touch_side == 0) &&
        ((m_effects.buttons & ORBIS_BTN_TOUCHPAD) != 0 && m_effects.touch_side == 1 &&
         (m_effects.buttons & ORBIS_BTN_RIGHT) == 0);

    return report;
}

} // namespace bbport::uwp::input
