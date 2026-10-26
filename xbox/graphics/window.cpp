// SPDX-License-Identifier: GPL-2.0-or-later
#include "sdl_window.h"
#include "bbport_overlay.h"
#include "bbport_prompts.h"
#include "gpu/bbgpu.h"
#include <SDL3/SDL.h>
#include <windows.h>
#include <chrono>
#include <mutex>
#include <thread>

extern "C" uint32_t runtime_pad_dialog_buttons(void);

namespace {
std::mutex g_text_dialog_mutex;
bool g_text_dialog_active = false;
int g_text_dialog_state = 0; // 0 typing, 1 confirmed, 2 cancelled
std::string g_text_dialog_value;
std::string g_text_dialog_prompt;
} // namespace

extern "C" int bb_uwp_dialog_key(uint32_t key, int down, int repeat) {
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
                if (key >= 'A' && key <= 'Z' && !(GetKeyState(VK_SHIFT) & 0x8000)) {
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
    BbPrompts::OnControllerConnected(BbPrompts::Xbox);
}

WindowSDL::~WindowSDL() = default;

void WindowSDL::UpdateTextTitle() {}

void WindowSDL::BeginTextInput(const std::string& initial, const std::string& prompt) {
    const std::string label = (prompt.empty() ? "Enter Hunter Name" : prompt) +
                              "  [A / Menu = Confirm, B = Cancel]";
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
    static thread_local uint32_t previous = 0;
    const uint32_t buttons = runtime_pad_dialog_buttons();
    const uint32_t changed = buttons ^ previous;
    previous = buttons;

    // Handle active IME text dialog (Character Creation Hunter Name) via Xbox controller.
    {
        std::scoped_lock lock(g_text_dialog_mutex);
        if (g_text_dialog_active) {
            // Confirm on new press of Cross (A, 0x4000) or Options (Menu, 0x8).
            if (((changed & 0x4000u) && (buttons & 0x4000u)) ||
                ((changed & 0x8u) && (buttons & 0x8u))) {
                if (g_text_dialog_value.empty()) g_text_dialog_value = "Hunter";
                g_text_dialog_state = 1;
                g_text_dialog_active = false;
                BbOverlay::SetTextEntry(false, {}, {});
            } else if ((changed & 0x2000u) && (buttons & 0x2000u)) {
                // Cancel on new press of Circle (B, 0x2000).
                g_text_dialog_state = 2;
                g_text_dialog_active = false;
                BbOverlay::SetTextEntry(false, {}, {});
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
    (void)target_bytes_to_free;
    return 0;
}

extern "C" {
void bbgpu_uwp_set_core_window(void*, int32_t, int32_t) {}
BbGpuBackend bbgpu_uwp_active_backend(void) { return BB_GPU_BACKEND_D3D12; }
void bbgpu_uwp_on_suspending(void) {}
void bbgpu_uwp_on_resuming(void) {}
uint64_t bbgpu_uwp_trim_memory(uint64_t) { return 0; }
}
