/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef BBGPU_H
#define BBGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} BbGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void bbgpu_register_kernel(void);
/* Co-op: the game folder and param.sfo identity for the network and PSN libraries. */
void bbnet_configure(const char *app0, const char *serial, const char *title, const char *app_ver);
/* The game's code before the patches (The Hunter's Dream backend), then the online session's
 * start (bbnet/bbnet_glue.cpp). */
void bbnet_set_image(unsigned char *image, uint64_t size);
void bbnet_initialize(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int bbgpu_init(const BbGpuConfig *config);
/* Flushes queued shader/pipeline cache blobs and driver cache to disk (UWP OnSuspending safe). */
void bbgpu_flush_caches(void);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t bbgpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int bbgpu_handle_fault(void *ucontext, void *address);
/* BB_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void bbgpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int bbgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int bbgpu_text_input_poll(char *out_utf8, uint64_t size);
/* Controller/keyboard modal choice dialog (launcher and save-slot selection).
 * poll returns -1 while open, -2 cancelled, or 0..count-1 when chosen. */
int bbgpu_choice_begin(const char *title, const char *const *items, int count, int focus);
int bbgpu_choice_poll(void);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int bbgpu_overlay_captures_input(void);
/* 1 while the game should be silent: bbport.ini mute_unfocused is on and the window does not
 * have focus. Safe from any thread (the audio ports read it for every buffer). */
int bbgpu_audio_muted(void);
/* Button prompts (gpu/shim/bbport_prompts.cpp): what each of the game's key-guide icons shows
 * on keyboard and mouse, one string per icon in bbport_prompt_icons.h's order ("e",
 * "lshift+leftbutton", "w a s d", "mouse", "" for nothing). Draws them; safe from any thread. */
/* The controller button each key-guide icon's action is bound to (input.ini names, "" for the
 * game's own), in the same order: the controller prompts follow rebinding. */
void bbgpu_prompts_configure_pad(const char *const *buttons, int count);
void bbgpu_prompts_configure(const char *const *inputs, int count);

/* Mouse input accumulated by the window thread since the last call; the call resets the
 * motion (dx/dy) but not held buttons. All zero (and captured=0) while capture is inactive
 * (mouse_to_joystick absent or off, no window focus, menu or text entry open). buttons uses
 * SDL_BUTTON_MASK() bits (bit 0 = SDL_BUTTON_LEFT, ...), NOT the SDL_BUTTON_* index runtime_
 * input_config.c stores for a mouse_left/mouse_right/... binding -- convert with
 * SDL_BUTTON_MASK(value) before comparing. wheel bit 0 up, 1 down, 2 left, 3 right: a 33 ms
 * pulse per step (MOU-007), already decayed by the time this call sees it.
 * See specs/keyboard-and-mouse/spec-design-keyboard-mouse-input.md section 4.5 (API-002). */
typedef struct {
    float dx, dy;
    uint32_t buttons;
    uint32_t wheel;
    int32_t captured;
    int32_t locked_on; /* the direct mouse camera sees a lock-on target (motion reaches dx/dy) */
} BbMouseInput;
void bbgpu_mouse_take(BbMouseInput *out);
/* Applies a just-(re)loaded input.ini to the window thread: whether mouse-to-joystick mode
 * exists at all (mouse_mode_available; MOU-001), and the current toggle/reload hotkey
 * scancodes (SDL_SCANCODE_UNKNOWN = none bound; HOT-002). Safe from any thread; takes effect
 * on the window thread's next PollEvents. */
void bbgpu_input_configure(int mouse_mode_available, int32_t toggle_scancode, int32_t reload_scancode);
/* 1 once after the reload hotkey (default F8) was pressed, then clears back to 0 -- the pad
 * thread polls this once per sample() (CFG-008) to know when to re-read input.ini. 0 if no
 * window exists yet. */
int bbgpu_input_reload_requested(void);
/* Direct mouse camera (mouse_camera = direct in input.ini): whether it is wanted, its
 * sensitivity multiplier, and the motion's per-axis multipliers (input_mouse_scale_x/_y: -1 to
 * invert, the vertical sensitivity) and whether the drawn position turns too (mouse_camera_sync).
 * Safe from any thread; applied on the window thread's next
 * PollEvents. */
void bbgpu_mouse_camera_configure(int direct, float sensitivity, float scale_x, float scale_y, int sync);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned bbgpu_symbol_count(void);

#ifdef BBPORT_UWP
#define BB_GPU_UWP_ABI_READY 1
/* Active graphics backend selected by the UWP host (Game Mode D3D12 vs App Mode D3D11 fallback). */
typedef enum {
    BB_GPU_BACKEND_NONE = 0,
    BB_GPU_BACKEND_VULKAN = 1,
    BB_GPU_BACKEND_D3D12 = 2,
    BB_GPU_BACKEND_D3D11_FALLBACK = 3,
} BbGpuBackend;

/* Supplies the CoreWindow IUnknown* before bbgpu_init() creates the DXGI swapchain. */
void bbgpu_uwp_set_core_window(void *core_window_unknown, int32_t width, int32_t height);
/* Returns the active graphics backend (BB_GPU_BACKEND_D3D12 in Game Mode, D3D11 fallback otherwise). */
BbGpuBackend bbgpu_uwp_active_backend(void);
/* UWP lifecycle hooks invoked from CoreApplication::Suspending / Resuming. */
void bbgpu_uwp_on_suspending(void);
void bbgpu_uwp_on_resuming(void);
/* Evicts cold TextureSetMemo descriptor slices and staging pools down to target_bytes. */
uint64_t bbgpu_uwp_trim_memory(uint64_t target_bytes_to_free);
/* bbport: one row per memory category, for the UWP 5 s heartbeat (main.cpp). Every field is a
 * byte count except the counts and the flags. Zero means "nothing of this kind", except
 * `available`, which is 0 until the rasterizer exists - a category that is 0 because it does not
 * exist yet must not look like a category that is 0 because it is empty. */
typedef struct BbGpuMemoryCats {
    uint64_t device_used;      /* VRAM the driver reports in use (VMA), 0 when it cannot report */
    uint64_t texture_bytes;    /* registered texture images, by their guest size */
    uint64_t texture_count;
    uint64_t dense_live;       /* dense buffer arenas backed now */
    uint64_t dense_retired;    /* replaced arenas the fence has not released yet */
    uint64_t arena_reserve_used;
    uint64_t fence_submitted;  /* scheduler tick submitted */
    uint64_t fence_completed;  /* tick the GPU has signalled */
    int32_t available;
} BbGpuMemoryCats;
/* Fills `out`; returns 1 when the GPU exists (0 leaves the struct zeroed). */
int32_t bbgpu_uwp_memory_cats(BbGpuMemoryCats *out);
/* The calling thread's current operation, for the stall report. `tag` must be a string literal
 * (it is stored as a pointer; nothing copies or frees it). */
void bbgpu_thread_op(const char *tag, uint64_t arg0, uint64_t arg1);
void bbgpu_thread_op_done(void);
/* Prints every thread with a tag: one line each, with how long it has been in that operation. */
void bbgpu_thread_op_dump(void);
/* B8: one line for an allocation of 32 MiB or more, or one that took 250 ms or more. `caller` and
 * `kind` are static literals; `usage_before` is the console's AppMemoryUsage before the call. */
void bbgpu_note_alloc(uint64_t size, const char *kind, const char *caller, uint64_t usage_before,
                      double elapsed_ms);
/* Returns 1 if shader precompilation is currently compiling an item, filling diagnostic fields. */
int bbgpu_uwp_active_precompile_diag(uint32_t *out_idx, uint32_t *out_total, uint32_t *out_stage,
                                     uint64_t *out_hash, uint64_t *out_partner_hash);
#endif

#ifdef __cplusplus
}
#endif
#endif
