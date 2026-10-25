// SPDX-License-Identifier: GPL-2.0-or-later
#include "GamepadInput.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#include <Xinput.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#endif

namespace bbport::uwp {

namespace {

uint8_t FloatStickToOrbisByte(double v) {
    const double clamped = std::clamp(v, -1.0, 1.0);
    const double scaled = (clamped + 1.0) * 127.5;
    return static_cast<uint8_t>(std::clamp(std::floor(scaled + 0.5), 0.0, 255.0));
}

void ApplyRadialDeadzone(double in_x, double in_y, float dz, double* out_x, double* out_y) {
    const double mag = std::hypot(in_x, in_y);
    const double deadzone = std::clamp(static_cast<double>(dz), 0.0, 0.85);
    if (mag <= deadzone || mag < 1e-6) {
        *out_x = 0.0;
        *out_y = 0.0;
        return;
    }
    const double normalized = std::min(1.0, (mag - deadzone) / (1.0 - deadzone));
    const double scale = normalized / mag;
    *out_x = std::clamp(in_x * scale, -1.0, 1.0);
    *out_y = std::clamp(in_y * scale, -1.0, 1.0);
}

struct RuntimePadState {
    std::mutex mutex;
    GamepadProfile profile{};
    GamepadInput::HotplugCallback hotplug_cb;
    bool initialized = false;
    bool prev_l3_r3 = false;
    uint32_t prev_setup_mask = 0;
    std::atomic<uint32_t> cached_count{0};
    std::string active_name = "Xbox Wireless Controller (Windows.Gaming.Input)";
#if defined(_WIN32)
    winrt::event_token added_token{};
    winrt::event_token removed_token{};
    winrt::Windows::Gaming::Input::Gamepad active_pad{nullptr};
#endif
};

RuntimePadState& State() {
    static RuntimePadState s;
    return s;
}

#if defined(_WIN32)
void EnsureWinRtApartment() {
    thread_local bool apartment_ready = false;
    if (!apartment_ready) {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (winrt::hresult_error const& err) {
            if (err.code() != RPC_E_CHANGED_MODE) {
                throw;
            }
        }
        apartment_ready = true;
    }
}

bool PollXInputUapReading(HostGamepadReading* out) {
    XINPUT_STATE xs{};
    for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user) {
        if (XInputGetState(user, &xs) == ERROR_SUCCESS) {
            const auto& gp = xs.Gamepad;
            uint32_t b = HOST_PAD_NONE;
            if (gp.wButtons & XINPUT_GAMEPAD_START)          b |= HOST_PAD_MENU;
            if (gp.wButtons & XINPUT_GAMEPAD_BACK)           b |= HOST_PAD_VIEW;
            if (gp.wButtons & XINPUT_GAMEPAD_A)              b |= HOST_PAD_A;
            if (gp.wButtons & XINPUT_GAMEPAD_B)              b |= HOST_PAD_B;
            if (gp.wButtons & XINPUT_GAMEPAD_X)              b |= HOST_PAD_X;
            if (gp.wButtons & XINPUT_GAMEPAD_Y)              b |= HOST_PAD_Y;
            if (gp.wButtons & XINPUT_GAMEPAD_DPAD_UP)        b |= HOST_PAD_DPAD_UP;
            if (gp.wButtons & XINPUT_GAMEPAD_DPAD_DOWN)      b |= HOST_PAD_DPAD_DOWN;
            if (gp.wButtons & XINPUT_GAMEPAD_DPAD_LEFT)      b |= HOST_PAD_DPAD_LEFT;
            if (gp.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT)     b |= HOST_PAD_DPAD_RIGHT;
            if (gp.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  b |= HOST_PAD_LEFT_SHOULDER;
            if (gp.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) b |= HOST_PAD_RIGHT_SHOULDER;
            if (gp.wButtons & XINPUT_GAMEPAD_LEFT_THUMB)     b |= HOST_PAD_LEFT_THUMB;
            if (gp.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)    b |= HOST_PAD_RIGHT_THUMB;

            out->buttons = b;
            out->left_thumb_x = std::clamp(gp.sThumbLX / 32767.0, -1.0, 1.0);
            out->left_thumb_y = std::clamp(gp.sThumbLY / 32767.0, -1.0, 1.0);
            out->right_thumb_x = std::clamp(gp.sThumbRX / 32767.0, -1.0, 1.0);
            out->right_thumb_y = std::clamp(gp.sThumbRY / 32767.0, -1.0, 1.0);
            out->left_trigger = gp.bLeftTrigger / 255.0;
            out->right_trigger = gp.bRightTrigger / 255.0;
            return true;
        }
    }
    return false;
}
#endif

} // namespace

MappedOrbisPad TranslateReadingToBloodborne(const HostGamepadReading& reading,
                                            const GamepadProfile& profile,
                                            bool* prev_l3_r3_held) {
    MappedOrbisPad out{};

    // 1. Face buttons (with optional Asian/JP Circle=confirm / Cross=dodge swap).
    const uint32_t btn_a = profile.swap_confirm_circle ? ORBIS_BTN_CIRCLE : ORBIS_BTN_CROSS;
    const uint32_t btn_b = profile.swap_confirm_circle ? ORBIS_BTN_CROSS  : ORBIS_BTN_CIRCLE;
    if (reading.buttons & HOST_PAD_A)              out.buttons |= btn_a;              // Dodge / Confirm
    if (reading.buttons & HOST_PAD_B)              out.buttons |= btn_b;              // Sprint / Backstep / Roll
    if (reading.buttons & HOST_PAD_X)              out.buttons |= ORBIS_BTN_SQUARE;   // Quick Item Use
    if (reading.buttons & HOST_PAD_Y)              out.buttons |= ORBIS_BTN_TRIANGLE; // Blood Vial Heal

    // 2. Bumpers & Thumbstick Clicks (Trick Weapon Transform L1, Light/Visceral R1, Jump L3, Lock-on R3).
    if (reading.buttons & HOST_PAD_LEFT_SHOULDER)  out.buttons |= ORBIS_BTN_L1;
    if (reading.buttons & HOST_PAD_RIGHT_SHOULDER) out.buttons |= ORBIS_BTN_R1;
    if (reading.buttons & HOST_PAD_LEFT_THUMB)     out.buttons |= ORBIS_BTN_L3;
    if (reading.buttons & HOST_PAD_RIGHT_THUMB)    out.buttons |= ORBIS_BTN_R3;
    if (reading.buttons & HOST_PAD_MENU)           out.buttons |= ORBIS_BTN_OPTIONS;

    // Elite Series 2 paddles convenience mapping: P1=Circle (Sprint), P2=Right Touchpad, P3=Left Touchpad, P4=L1 (Transform).
    if (reading.buttons & HOST_PAD_PADDLE1)        out.buttons |= ORBIS_BTN_CIRCLE;
    if (reading.buttons & HOST_PAD_PADDLE4)        out.buttons |= ORBIS_BTN_L1;

    // 3. DualShock 4 Touchpad Emulation via View (Back) chords:
    //    - View + DPadRight (or Paddle2) -> Right Touchpad half (x=1440, y=471: Personal Effects)
    //    - View + DPadLeft  (or View alone / Paddle3) -> Left Touchpad half (x=480, y=471: Gestures)
    //    Suppresses DPadLeft/DPadRight weapon swaps while View is held so chords never misfire!
    const bool view_held = (reading.buttons & HOST_PAD_VIEW) != 0;
    if (reading.buttons & HOST_PAD_PADDLE2) {
        out.buttons |= ORBIS_BTN_TOUCHPAD;
        out.touch_side = 1;
    } else if (reading.buttons & HOST_PAD_PADDLE3) {
        out.buttons |= ORBIS_BTN_TOUCHPAD;
        out.touch_side = 0;
    } else if (view_held) {
        out.buttons |= ORBIS_BTN_TOUCHPAD;
        if (reading.buttons & HOST_PAD_DPAD_RIGHT) {
            out.touch_side = 1; // Right half: Personal Effects
        } else {
            out.touch_side = 0; // Left half: Gestures (View alone or View + DPadLeft)
        }
    }

    // 4. D-Pad (Blood Bullets Up, Cycle Item Down, Firearm Left, Trick Weapon Right).
    if (reading.buttons & HOST_PAD_DPAD_UP)                    out.buttons |= ORBIS_BTN_UP;
    if (reading.buttons & HOST_PAD_DPAD_DOWN)                  out.buttons |= ORBIS_BTN_DOWN;
    if (!view_held && (reading.buttons & HOST_PAD_DPAD_LEFT))  out.buttons |= ORBIS_BTN_LEFT;
    if (!view_held && (reading.buttons & HOST_PAD_DPAD_RIGHT)) out.buttons |= ORBIS_BTN_RIGHT;

    // 5. Analog Triggers (L2 Firearm Parry, R2 Heavy / Charged Backstab).
    out.axes[4] = static_cast<uint8_t>(std::clamp(std::floor(reading.left_trigger * 255.0 + 0.5), 0.0, 255.0));
    out.axes[5] = static_cast<uint8_t>(std::clamp(std::floor(reading.right_trigger * 255.0 + 0.5), 0.0, 255.0));
    if (out.axes[4] > profile.trigger_threshold) out.buttons |= ORBIS_BTN_L2;
    if (out.axes[5] > profile.trigger_threshold) out.buttons |= ORBIS_BTN_R2;

    // 6. Analog Sticks: radial deadzone + Y-axis inversion (WinRT +Y=up -> Orbis 0=up, 255=down).
    double lx = 0.0, ly = 0.0, rx = 0.0, ry = 0.0;
    ApplyRadialDeadzone(reading.left_thumb_x, reading.left_thumb_y, profile.left_deadzone, &lx, &ly);
    ApplyRadialDeadzone(reading.right_thumb_x, reading.right_thumb_y, profile.right_deadzone, &rx, &ry);
    out.axes[0] = FloatStickToOrbisByte(lx);
    out.axes[1] = FloatStickToOrbisByte(-ly);
    out.axes[2] = FloatStickToOrbisByte(rx);
    out.axes[3] = FloatStickToOrbisByte(-ry);

    // 7. L3 + R3 overlay chord edge detection (matches desktop Insert / L3+R3 menu shortcut).
    const bool l3_r3_now = profile.enable_l3_r3_overlay_chord &&
                           ((out.buttons & (ORBIS_BTN_L3 | ORBIS_BTN_R3)) == (ORBIS_BTN_L3 | ORBIS_BTN_R3));
    if (prev_l3_r3_held) {
        out.overlay_toggle_edge = l3_r3_now && !(*prev_l3_r3_held);
        *prev_l3_r3_held = l3_r3_now;
    }

    return out;
}

GamepadInput& GamepadInput::Instance() {
    static GamepadInput inst;
    return inst;
}

void GamepadInput::Initialize() {
    auto& st = State();
    std::scoped_lock lock(st.mutex);
    if (st.initialized) {
        return;
    }
#if defined(_WIN32)
    EnsureWinRtApartment();
    using winrt::Windows::Gaming::Input::Gamepad;

    st.added_token = Gamepad::GamepadAdded([](auto const&, Gamepad const& added) {
        auto& s = State();
        HotplugCallback cb;
        uint32_t new_count = 0;
        {
            std::scoped_lock lk(s.mutex);
            if (!s.active_pad) {
                s.active_pad = added;
            }
            new_count = Gamepad::Gamepads().Size();
            s.cached_count.store(new_count, std::memory_order_relaxed);
            cb = s.hotplug_cb;
        }
        std::printf("UWP GamepadInput: controller connected (count=%u)\n", new_count);
        if (cb) cb(new_count);
    });

    st.removed_token = Gamepad::GamepadRemoved([](auto const&, Gamepad const& removed) {
        auto& s = State();
        HotplugCallback cb;
        uint32_t new_count = 0;
        {
            std::scoped_lock lk(s.mutex);
            auto pads = Gamepad::Gamepads();
            new_count = pads.Size();
            if (s.active_pad == removed) {
                s.active_pad = new_count > 0 ? pads.GetAt(0) : Gamepad{nullptr};
            }
            s.cached_count.store(new_count, std::memory_order_relaxed);
            cb = s.hotplug_cb;
        }
        std::printf("UWP GamepadInput: controller disconnected (count=%u)\n", new_count);
        if (cb) cb(new_count);
    });

    auto pads = Gamepad::Gamepads();
    st.cached_count.store(pads.Size(), std::memory_order_relaxed);
    if (pads.Size() > 0) {
        st.active_pad = pads.GetAt(0);
    }
#endif
    st.initialized = true;
}

void GamepadInput::Shutdown() {
    auto& st = State();
    std::scoped_lock lock(st.mutex);
    if (!st.initialized) {
        return;
    }
#if defined(_WIN32)
    using winrt::Windows::Gaming::Input::Gamepad;
    if (st.added_token.value != 0) {
        Gamepad::GamepadAdded(st.added_token);
        st.added_token = {};
    }
    if (st.removed_token.value != 0) {
        Gamepad::GamepadRemoved(st.removed_token);
        st.removed_token = {};
    }
    st.active_pad = nullptr;
#endif
    st.initialized = false;
}

void GamepadInput::SetProfile(const GamepadProfile& profile) {
    auto& st = State();
    std::scoped_lock lock(st.mutex);
    st.profile = profile;
}

GamepadProfile GamepadInput::GetProfile() const {
    auto& st = State();
    std::scoped_lock lock(st.mutex);
    return st.profile;
}

bool GamepadInput::PollBloodborneState(MappedOrbisPad* out_state, uint8_t* out_connected_count) {
    if (!out_state) {
        return false;
    }
    Initialize();

    auto& st = State();
    HostGamepadReading raw{};
    bool have_reading = false;

#if defined(_WIN32)
    EnsureWinRtApartment();
    GamepadProfile profile{};
    {
        std::scoped_lock lock(st.mutex);
        profile = st.profile;
    }
    if (!profile.prefer_xinput_uap) {
        using winrt::Windows::Gaming::Input::Gamepad;
        using winrt::Windows::Gaming::Input::GamepadButtons;
        winrt::Windows::Gaming::Input::Gamepad pad{nullptr};
        uint32_t pad_count = 0;
        {
            std::scoped_lock lock(st.mutex);
            auto pads = Gamepad::Gamepads();
            pad_count = pads.Size();
            st.cached_count.store(pad_count, std::memory_order_relaxed);
            if (!st.active_pad && pad_count > 0) {
                st.active_pad = pads.GetAt(0);
            }
            pad = st.active_pad;
        }
        if (out_connected_count) {
            *out_connected_count = static_cast<uint8_t>(std::min<uint32_t>(pad_count, 4u));
        }
        if (pad) {
            const auto r = pad.GetCurrentReading();
            const auto b = r.Buttons;
            if ((b & GamepadButtons::Menu) != GamepadButtons::None)            raw.buttons |= HOST_PAD_MENU;
            if ((b & GamepadButtons::View) != GamepadButtons::None)            raw.buttons |= HOST_PAD_VIEW;
            if ((b & GamepadButtons::A) != GamepadButtons::None)               raw.buttons |= HOST_PAD_A;
            if ((b & GamepadButtons::B) != GamepadButtons::None)               raw.buttons |= HOST_PAD_B;
            if ((b & GamepadButtons::X) != GamepadButtons::None)               raw.buttons |= HOST_PAD_X;
            if ((b & GamepadButtons::Y) != GamepadButtons::None)               raw.buttons |= HOST_PAD_Y;
            if ((b & GamepadButtons::DPadUp) != GamepadButtons::None)          raw.buttons |= HOST_PAD_DPAD_UP;
            if ((b & GamepadButtons::DPadDown) != GamepadButtons::None)        raw.buttons |= HOST_PAD_DPAD_DOWN;
            if ((b & GamepadButtons::DPadLeft) != GamepadButtons::None)        raw.buttons |= HOST_PAD_DPAD_LEFT;
            if ((b & GamepadButtons::DPadRight) != GamepadButtons::None)       raw.buttons |= HOST_PAD_DPAD_RIGHT;
            if ((b & GamepadButtons::LeftShoulder) != GamepadButtons::None)    raw.buttons |= HOST_PAD_LEFT_SHOULDER;
            if ((b & GamepadButtons::RightShoulder) != GamepadButtons::None)   raw.buttons |= HOST_PAD_RIGHT_SHOULDER;
            if ((b & GamepadButtons::LeftThumbstick) != GamepadButtons::None)  raw.buttons |= HOST_PAD_LEFT_THUMB;
            if ((b & GamepadButtons::RightThumbstick) != GamepadButtons::None) raw.buttons |= HOST_PAD_RIGHT_THUMB;
            if ((b & GamepadButtons::Paddle1) != GamepadButtons::None)         raw.buttons |= HOST_PAD_PADDLE1;
            if ((b & GamepadButtons::Paddle2) != GamepadButtons::None)         raw.buttons |= HOST_PAD_PADDLE2;
            if ((b & GamepadButtons::Paddle3) != GamepadButtons::None)         raw.buttons |= HOST_PAD_PADDLE3;
            if ((b & GamepadButtons::Paddle4) != GamepadButtons::None)         raw.buttons |= HOST_PAD_PADDLE4;

            raw.left_thumb_x = r.LeftThumbstickX;
            raw.left_thumb_y = r.LeftThumbstickY;
            raw.right_thumb_x = r.RightThumbstickX;
            raw.right_thumb_y = r.RightThumbstickY;
            raw.left_trigger = r.LeftTrigger;
            raw.right_trigger = r.RightTrigger;
            raw.timestamp_us = r.Timestamp;
            have_reading = true;
        }
    }

    if (!have_reading && PollXInputUapReading(&raw)) {
        have_reading = true;
        if (out_connected_count && *out_connected_count == 0) {
            *out_connected_count = 1;
        }
    }
#else
    if (out_connected_count) {
        *out_connected_count = 0;
    }
#endif

    if (!have_reading) {
        *out_state = MappedOrbisPad{};
        return false;
    }

    std::scoped_lock lock(st.mutex);
    *out_state = TranslateReadingToBloodborne(raw, st.profile, &st.prev_l3_r3);
    return true;
}

std::string GamepadInput::PollLauncherSetupButton() {
    MappedOrbisPad pad{};
    uint8_t count = 0;
    if (!PollBloodborneState(&pad, &count)) {
        return {};
    }
    auto& st = State();
    std::scoped_lock lock(st.mutex);
    const uint32_t newly_pressed = pad.buttons & ~st.prev_setup_mask;
    st.prev_setup_mask = pad.buttons;

    struct TokenEntry { uint32_t bit; const char* name; };
    static constexpr TokenEntry kTokens[] = {
        {ORBIS_BTN_CROSS,    "cross"},
        {ORBIS_BTN_CIRCLE,   "circle"},
        {ORBIS_BTN_SQUARE,   "square"},
        {ORBIS_BTN_TRIANGLE, "triangle"},
        {ORBIS_BTN_L1,       "l1"},
        {ORBIS_BTN_R1,       "r1"},
        {ORBIS_BTN_L2,       "l2"},
        {ORBIS_BTN_R2,       "r2"},
        {ORBIS_BTN_L3,       "l3"},
        {ORBIS_BTN_R3,       "r3"},
        {ORBIS_BTN_OPTIONS,  "options"},
        {ORBIS_BTN_TOUCHPAD, "back"},
        {ORBIS_BTN_UP,       "pad_up"},
        {ORBIS_BTN_DOWN,     "pad_down"},
        {ORBIS_BTN_LEFT,     "pad_left"},
        {ORBIS_BTN_RIGHT,    "pad_right"},
    };
    for (const auto& entry : kTokens) {
        if (newly_pressed & entry.bit) {
            return entry.name;
        }
    }
    return {};
}

void GamepadInput::SetVibration(uint8_t large_motor, uint8_t small_motor) {
#if defined(_WIN32)
    EnsureWinRtApartment();
    auto& st = State();
    GamepadProfile profile{};
    winrt::Windows::Gaming::Input::Gamepad pad{nullptr};
    {
        std::scoped_lock lock(st.mutex);
        profile = st.profile;
        pad = st.active_pad;
    }
    if (!profile.prefer_xinput_uap && pad) {
        winrt::Windows::Gaming::Input::GamepadVibration vib{};
        vib.LeftMotor = std::clamp(large_motor / 255.0, 0.0, 1.0);
        vib.RightMotor = std::clamp(small_motor / 255.0, 0.0, 1.0);
        vib.LeftTrigger = 0.0;
        vib.RightTrigger = 0.0;
        pad.Vibration(vib);
        return;
    }
    XINPUT_VIBRATION xv{};
    xv.wLeftMotorSpeed = static_cast<WORD>(large_motor) * 257u;
    xv.wRightMotorSpeed = static_cast<WORD>(small_motor) * 257u;
    XInputSetState(0, &xv);
#else
    (void)large_motor;
    (void)small_motor;
#endif
}

uint32_t GamepadInput::ConnectedCount() const {
    return State().cached_count.load(std::memory_order_relaxed);
}

std::string GamepadInput::ActiveControllerName() const {
    auto& st = State();
    std::scoped_lock lock(st.mutex);
    return st.active_name;
}

void GamepadInput::SetHotplugCallback(HotplugCallback cb) {
    auto& st = State();
    std::scoped_lock lock(st.mutex);
    st.hotplug_cb = std::move(cb);
}

} // namespace bbport::uwp

extern "C" int bbport_uwp_gamepad_poll(uint32_t* out_buttons, uint8_t out_axes[6],
                                       int* out_touch_side, uint8_t* out_connected_count) {
    bbport::uwp::MappedOrbisPad mapped{};
    uint8_t count = 0;
    const bool ok = bbport::uwp::GamepadInput::Instance().PollBloodborneState(&mapped, &count);
    if (out_buttons) *out_buttons = mapped.buttons;
    if (out_axes) {
        for (int i = 0; i < 6; ++i) out_axes[i] = mapped.axes[i];
    }
    if (out_touch_side) *out_touch_side = mapped.touch_side;
    if (out_connected_count) *out_connected_count = count;
    return ok ? 1 : 0;
}

extern "C" void bbport_uwp_gamepad_set_vibration(uint8_t large_motor, uint8_t small_motor) {
    bbport::uwp::GamepadInput::Instance().SetVibration(large_motor, small_motor);
}

extern "C" const char* bbport_uwp_gamepad_name(void) {
    static std::string cached;
    cached = bbport::uwp::GamepadInput::Instance().ActiveControllerName();
    return cached.c_str();
}
