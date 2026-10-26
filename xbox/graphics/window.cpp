// SPDX-License-Identifier: GPL-2.0-or-later
#include "sdl_window.h"
#include "bbport_overlay.h"
#include "bbport_prompts.h"
#include "bbport_settings.h"
#include "gpu/bbgpu.h"
#include <SDL3/SDL.h>
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

extern "C" uint32_t runtime_pad_dialog_buttons(void);
extern "C" int bbport_uwp_gamepad_poll(uint32_t* out_buttons, uint8_t out_axes[6],
                                       int* out_touch_side, uint8_t* out_connected_count);
extern "C" int bb_uwp_app_visible(void);
extern "C" size_t runtime_memory_page_out(size_t target_bytes);

namespace {
std::mutex g_text_dialog_mutex;
bool g_text_dialog_active = false;
bool g_text_dialog_wait_release = false;
int g_text_dialog_state = 0; // 0 typing, 1 confirmed, 2 cancelled
std::string g_text_dialog_value;
std::string g_text_dialog_prompt;
bool g_shift_down = false;

constexpr char kControllerCharset[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -";
constexpr size_t kControllerCharsetLen = sizeof(kControllerCharset) - 1;

char CycleControllerChar(char current, int delta) {
    size_t idx = 0;
    for (size_t i = 0; i < kControllerCharsetLen; ++i) {
        if (kControllerCharset[i] == current) {
            idx = i;
            break;
        }
    }
    idx = (idx + kControllerCharsetLen + static_cast<size_t>(delta)) % kControllerCharsetLen;
    return kControllerCharset[idx];
}
} // namespace

extern "C" int bb_uwp_dialog_key(uint32_t key, int down, int repeat) {
    if (key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT) {
        g_shift_down = (down != 0);
        return 0;
    }
    {
        std::scoped_lock lock(g_text_dialog_mutex);
        if (g_text_dialog_active) {
            if (!down) return 1;
            if (key == VK_RETURN) {
                if (g_text_dialog_value.empty()) g_text_dialog_value = "Hunter";
                g_text_dialog_state = 1;
                g_text_dialog_active = false;
                BbOverlay::SetTextEntry(false, {}, {});
                return 1;
            }
            if (key == VK_ESCAPE) {
                g_text_dialog_state = 2;
                g_text_dialog_active = false;
                BbOverlay::SetTextEntry(false, {}, {});
                return 1;
            }
            if (key == VK_BACK && !g_text_dialog_value.empty()) {
                g_text_dialog_value.pop_back();
                BbOverlay::SetTextEntry(true, g_text_dialog_prompt, g_text_dialog_value);
                return 1;
            }
            if (!repeat && key >= 0x20 && key <= 0x7E && g_text_dialog_value.size() < 16) {
                char c = static_cast<char>(key);
                if (key >= 'A' && key <= 'Z' && !g_shift_down) {
                    c = static_cast<char>(c + ('a' - 'A'));
                }
                g_text_dialog_value.push_back(c);
                BbOverlay::SetTextEntry(true, g_text_dialog_prompt, g_text_dialog_value);
                return 1;
            }
            return 1;
        }
    }

    // Insert opens the menu as on PC; other keys only work while the menu or a dialog is open.
    if ((key != VK_INSERT && !BbOverlay::CapturesInput()) || (down && repeat)) return 0;
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    switch (key) {
    case VK_INSERT: event.key.key = SDLK_INSERT; break;
    case VK_LEFT: event.key.key = SDLK_LEFT; break;
    case VK_RIGHT: event.key.key = SDLK_RIGHT; break;
    case VK_RETURN: event.key.key = SDLK_RETURN; break;
    case VK_SPACE: event.key.key = SDLK_SPACE; break;
    case VK_ESCAPE: event.key.key = SDLK_ESCAPE; break;
    case VK_BACK: event.key.key = SDLK_BACKSPACE; break;
    case VK_UP: event.key.key = SDLK_UP; break;
    case VK_DOWN: event.key.key = SDLK_DOWN; break;
    case 'W': event.key.key = SDLK_W; break;
    case 'S': event.key.key = SDLK_S; break;
    default: return 0;
    }
    return BbOverlay::HandleEvent(event);
}

namespace Frontend {

WindowSDL::WindowSDL(s32 w, s32 h, const char* title) : width(w), height(h), base_title(title ? title : "Bloodborne") {
    const int out = std::clamp(BbSettings::Get().output_res.load(std::memory_order_relaxed), 0,
                               BbSettings::XboxOutputCount - 1);
    width.store(BbSettings::OutputWidths[out], std::memory_order_relaxed);
    height.store(BbSettings::OutputHeights[out], std::memory_order_relaxed);
    window_focused.store(true, std::memory_order_relaxed);
    BbPrompts::OnControllerConnected(BbPrompts::Xbox);
}

WindowSDL::~WindowSDL() = default;

void WindowSDL::UpdateTextTitle() {}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    const std::string label = prompt.empty() ? "Enter Hunter Name" : prompt;
    const std::string start_value = initial.empty() ? "Hunter" : initial;
    {
        std::scoped_lock lock(text_mutex);
        text = start_value;
        text_prompt = label;
        text_state = 0;
    }
    {
        std::scoped_lock lock(g_text_dialog_mutex);
        g_text_dialog_value = start_value;
        g_text_dialog_prompt = label;
        g_text_dialog_state = 0;
        g_text_dialog_wait_release = true;
        g_text_dialog_active = true;
    }
    BbOverlay::SetTextEntry(true, label, start_value);
}

int WindowSDL::PollTextInput(std::string& result) {
    std::scoped_lock lock(g_text_dialog_mutex);
    result = g_text_dialog_value;
    return g_text_dialog_state;
}

bool WindowSDL::PollEvents() {
    window_focused.store(bb_uwp_app_visible() != 0, std::memory_order_relaxed);
    {
        const int out = std::clamp(BbSettings::Get().output_res.load(std::memory_order_relaxed), 0,
                                   BbSettings::XboxOutputCount - 1);
        const s32 want_w = BbSettings::OutputWidths[out];
        const s32 want_h = BbSettings::OutputHeights[out];
        if (width.load(std::memory_order_relaxed) != want_w ||
            height.load(std::memory_order_relaxed) != want_h) {
            width.store(want_w, std::memory_order_relaxed);
            height.store(want_h, std::memory_order_relaxed);
        }
    }

    static thread_local uint32_t previous = 0;
    uint32_t buttons = 0;
    uint8_t axes[6] = {128, 128, 128, 128, 0, 0};
    int touch_side = -1;
    uint8_t pad_count = 1;
    bbport_uwp_gamepad_poll(&buttons, axes, &touch_side, &pad_count);
    if (BbOverlay::CapturesInput()) {
        // Map Left Thumbstick deflections to virtual D-Pad directions for smooth Xbox menu nav.
        if (axes[1] < 56) buttons |= 0x10u;       // Up
        else if (axes[1] > 200) buttons |= 0x40u; // Down
        if (axes[0] < 56) buttons |= 0x80u;       // Left
        else if (axes[0] > 200) buttons |= 0x20u; // Right
    }
    const uint32_t changed = buttons ^ previous;
    previous = buttons;

    // Handle active IME text dialog (Character Creation Hunter Name) via Xbox controller.
    {
        std::scoped_lock lock(g_text_dialog_mutex);
        if (g_text_dialog_active) {
            constexpr uint32_t kDialogMask =
                0x4000u | 0x2000u | 0x8000u | 0x8u | 0x10u | 0x20u | 0x40u | 0x80u;
            if (g_text_dialog_wait_release) {
                if ((buttons & kDialogMask) == 0) {
                    g_text_dialog_wait_release = false;
                }
            } else {
                const auto pressed = [&](uint32_t bit) {
                    return ((changed & bit) != 0) && ((buttons & bit) != 0);
                };
                // Confirm on new press of Cross (A, 0x4000) or Options (Menu, 0x8).
                if (pressed(0x4000u) || pressed(0x8u)) {
                    if (g_text_dialog_value.empty()) g_text_dialog_value = "Hunter";
                    g_text_dialog_state = 1;
                    g_text_dialog_active = false;
                    BbOverlay::SetTextEntry(false, {}, {});
                } else if (pressed(0x2000u)) {
                    // Cancel on new press of Circle (B, 0x2000).
                    g_text_dialog_state = 2;
                    g_text_dialog_active = false;
                    BbOverlay::SetTextEntry(false, {}, {});
                } else if (pressed(0x10u) || pressed(0x40u)) {
                    // D-Pad Up / Down cycles the trailing letter.
                    if (g_text_dialog_value.empty()) {
                        g_text_dialog_value = "A";
                    } else {
                        g_text_dialog_value.back() =
                            CycleControllerChar(g_text_dialog_value.back(),
                                                pressed(0x10u) ? 1 : -1);
                    }
                    BbOverlay::SetTextEntry(true, g_text_dialog_prompt, g_text_dialog_value);
                } else if (pressed(0x20u)) {
                    // D-Pad Right appends a character (up to 16 chars).
                    if (g_text_dialog_value.size() < 16) {
                        g_text_dialog_value.push_back(g_text_dialog_value.empty() ? 'A' : 'a');
                        BbOverlay::SetTextEntry(true, g_text_dialog_prompt, g_text_dialog_value);
                    }
                } else if (pressed(0x80u) || pressed(0x8000u)) {
                    // D-Pad Left or X deletes the trailing character.
                    if (!g_text_dialog_value.empty()) {
                        g_text_dialog_value.pop_back();
                        BbOverlay::SetTextEntry(true, g_text_dialog_prompt, g_text_dialog_value);
                    }
                }
            }
            BbPrompts::Apply();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            return IsOpen();
        }
    }

    const auto send = [&](uint32_t guest, Uint8 host) {
        SDL_Event event{};
        event.type = (buttons & guest) ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        event.gbutton.button = host;
        BbOverlay::HandleEvent(event);
    };

    // Bloodborne overlay opens and closes on L3 + R3 (guest 0x2 + 0x4), so both always reach it.
    if (changed & 0x2u) send(0x2u, SDL_GAMEPAD_BUTTON_LEFT_STICK);
    if (changed & 0x4u) send(0x4u, SDL_GAMEPAD_BUTTON_RIGHT_STICK);

    if (BbOverlay::CapturesInput()) {
        struct Mapping {
            uint32_t guest;
            Uint8 host;
        };
        constexpr Mapping map[] = {
            {0x4000u, SDL_GAMEPAD_BUTTON_SOUTH},
            {0x2000u, SDL_GAMEPAD_BUTTON_EAST},
            {0x8000u, SDL_GAMEPAD_BUTTON_WEST},
            {0x1000u, SDL_GAMEPAD_BUTTON_NORTH},
            {0x10u, SDL_GAMEPAD_BUTTON_DPAD_UP},
            {0x40u, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
            {0x80u, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
            {0x20u, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
            {0x400u, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
            {0x800u, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
            {0x8u, SDL_GAMEPAD_BUTTON_START},
            {0x100000u, SDL_GAMEPAD_BUTTON_BACK},
        };
        for (const auto& entry : map) {
            if (changed & entry.guest) {
                send(entry.guest, entry.host);
                // One press closes at most one modal dialog.
                if (!BbOverlay::CapturesInput()) break;
            }
        }
    }

    BbPrompts::OnInput(BbPrompts::Xbox);
    BbPrompts::Apply();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return IsOpen();
}

void WindowSDL::TakeMouseInput(MouseInput& out) {
    std::scoped_lock lock(mouse_mutex);
    out = MouseInput{};
}

void WindowSDL::ConfigureInput(bool, int32_t, int32_t) {}

int WindowSDL::TakeInputReloadRequested() {
    return 0;
}

} // namespace Frontend

extern "C" void bbgpu_uwp_set_core_window(void* core_window_unknown, int32_t width, int32_t height) {
    (void)core_window_unknown;
    (void)width;
    (void)height;
}

extern "C" BbGpuBackend bbgpu_uwp_active_backend(void) {
    return BB_GPU_BACKEND_D3D12;
}

extern "C" void bbgpu_uwp_on_suspending(void) {}
extern "C" void bbgpu_uwp_on_resuming(void) {}
extern "C" uint64_t bbgpu_uwp_trim_memory(uint64_t target_bytes_to_free) {
    return static_cast<uint64_t>(
        runtime_memory_page_out(static_cast<size_t>(target_bytes_to_free ? target_bytes_to_free
                                                                         : (64ULL << 20))));
}
