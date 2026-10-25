// SPDX-License-Identifier: GPL-2.0-or-later
// Native Xbox / UWP Controller Layer using Windows.Gaming.Input.Gamepad + xinput_uap.lib fallback.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace bbport::uwp {

// PS4 OrbisPad digital bitmask values matching src/runtime_pad.c exactly.
enum OrbisButton : uint32_t {
    ORBIS_BTN_L3       = 0x000002u,
    ORBIS_BTN_R3       = 0x000004u,
    ORBIS_BTN_OPTIONS  = 0x000008u,
    ORBIS_BTN_UP       = 0x000010u,
    ORBIS_BTN_RIGHT    = 0x000020u,
    ORBIS_BTN_DOWN     = 0x000040u,
    ORBIS_BTN_LEFT     = 0x000080u,
    ORBIS_BTN_L2       = 0x000100u,
    ORBIS_BTN_R2       = 0x000200u,
    ORBIS_BTN_L1       = 0x000400u,
    ORBIS_BTN_R1       = 0x000800u,
    ORBIS_BTN_TRIANGLE = 0x001000u,
    ORBIS_BTN_CIRCLE   = 0x002000u,
    ORBIS_BTN_CROSS    = 0x004000u,
    ORBIS_BTN_SQUARE   = 0x008000u,
    ORBIS_BTN_TOUCHPAD = 0x100000u,
};

// Host-normalized gamepad state matching Windows.Gaming.Input.GamepadReading semantics.
enum HostGamepadButtons : uint32_t {
    HOST_PAD_NONE            = 0,
    HOST_PAD_MENU            = 1u << 0,
    HOST_PAD_VIEW            = 1u << 1,
    HOST_PAD_A               = 1u << 2,
    HOST_PAD_B               = 1u << 3,
    HOST_PAD_X               = 1u << 4,
    HOST_PAD_Y               = 1u << 5,
    HOST_PAD_DPAD_UP         = 1u << 6,
    HOST_PAD_DPAD_DOWN       = 1u << 7,
    HOST_PAD_DPAD_LEFT       = 1u << 8,
    HOST_PAD_DPAD_RIGHT      = 1u << 9,
    HOST_PAD_LEFT_SHOULDER   = 1u << 10,
    HOST_PAD_RIGHT_SHOULDER  = 1u << 11,
    HOST_PAD_LEFT_THUMB      = 1u << 12,
    HOST_PAD_RIGHT_THUMB     = 1u << 13,
    HOST_PAD_PADDLE1         = 1u << 14,
    HOST_PAD_PADDLE2         = 1u << 15,
    HOST_PAD_PADDLE3         = 1u << 16,
    HOST_PAD_PADDLE4         = 1u << 17,
};

struct HostGamepadReading {
    uint32_t buttons = HOST_PAD_NONE;
    double left_thumb_x = 0.0;   // [-1.0, +1.0], +X = right
    double left_thumb_y = 0.0;   // [-1.0, +1.0], +Y = up (WinRT convention)
    double right_thumb_x = 0.0;  // [-1.0, +1.0], +X = right
    double right_thumb_y = 0.0;  // [-1.0, +1.0], +Y = up (WinRT convention)
    double left_trigger = 0.0;   // [0.0, 1.0]
    double right_trigger = 0.0;  // [0.0, 1.0]
    uint64_t timestamp_us = 0;
};

struct GamepadProfile {
    float left_deadzone = 0.12f;
    float right_deadzone = 0.12f;
    uint8_t trigger_threshold = 30;  // matches src/runtime_input_config.h
    bool swap_confirm_circle = false; // Asian/JP Circle=confirm, Cross=dodge
    bool prefer_xinput_uap = false;   // fallback via xinput_uap.lib when requested
    bool enable_l3_r3_overlay_chord = true;
};

struct MappedOrbisPad {
    uint32_t buttons = 0;
    uint8_t axes[6] = {128, 128, 128, 128, 0, 0}; // lx, ly, rx, ry, l2, r2
    int touch_side = -1; // -1 none, 0 left (gestures x=480), 1 right (personal effects x=1440)
    bool overlay_toggle_edge = false;
};

// Pure translation function (deterministic, unit-tested on both Windows UWP and host CI).
MappedOrbisPad TranslateReadingToBloodborne(const HostGamepadReading& reading,
                                            const GamepadProfile& profile,
                                            bool* prev_l3_r3_held = nullptr);

class GamepadInput {
public:
    static GamepadInput& Instance();

    void Initialize();
    void Shutdown();

    void SetProfile(const GamepadProfile& profile);
    GamepadProfile GetProfile() const;

    // Polls Windows.Gaming.Input.Gamepad (or xinput_uap.lib fallback) and maps to OrbisPadData.
    bool PollBloodborneState(MappedOrbisPad* out_state, uint8_t* out_connected_count);

    // Captures a single newly pressed button token ("cross", "circle", "l1", "r2", etc.)
    // for the UWP LauncherPage controller setup flow without any desktop SDL3.dll dependency.
    std::string PollLauncherSetupButton();

    void SetVibration(uint8_t large_motor, uint8_t small_motor);
    uint32_t ConnectedCount() const;
    std::string ActiveControllerName() const;

    using HotplugCallback = std::function<void(uint32_t connected_count)>;
    void SetHotplugCallback(HotplugCallback cb);

private:
    GamepadInput() = default;
    ~GamepadInput() = default;
    GamepadInput(const GamepadInput&) = delete;
    GamepadInput& operator=(const GamepadInput&) = delete;
};

} // namespace bbport::uwp

#ifdef __cplusplus
extern "C" {
#endif
int bbport_uwp_gamepad_poll(uint32_t* out_buttons, uint8_t out_axes[6],
                            int* out_touch_side, uint8_t* out_connected_count);
void bbport_uwp_gamepad_set_vibration(uint8_t large_motor, uint8_t small_motor);
const char* bbport_uwp_gamepad_name(void);
#ifdef __cplusplus
}
#endif
