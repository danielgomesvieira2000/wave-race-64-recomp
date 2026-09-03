// Phase 03: the platform callbacks ultramodern asks the project to supply.
//
// ultramodern reimplements libultra but deliberately owns no platform I/O, so
// everything that touches a window, a gamepad, a speaker or an OS dialog is
// provided from here. SDL2 does the work; RT64 already vendors and links it.
//
// Two of these are honest placeholders rather than implementations, and are
// marked as such where they appear: audio output and RSP microcode. Both are
// phase 05 work, and neither is needed to reach the boot gate.

#include "wr64/callbacks.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <SDL.h>
#if defined(_WIN32)
#   include <SDL_syswm.h>
#endif

#include <ultramodern/ultramodern.hpp>
#include <ultramodern/input.hpp>
#include <ultramodern/rsp.hpp>
#include <ultramodern/events.hpp>
#include <ultramodern/error_handling.hpp>
#include <ultramodern/threads.hpp>
#include <librecomp/rsp.hpp>

#include "wr64/renderer.h"

namespace {

SDL_Window* g_window = nullptr;
SDL_GameController* g_controller = nullptr;

// ---------------------------------------------------------------- input ----

void poll_input() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                // Traced during phase 04: a clean exit-code-0 shutdown and a
                // crash look the same from outside, so it matters whether the
                // quit came from here or from the runtime deciding to stop.
                std::fprintf(stderr, "[wr64] SDL_QUIT received; asking the runtime to quit\n");
                std::fflush(stderr);
                ultramodern::quit();
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (g_controller == nullptr) {
                    g_controller = SDL_GameControllerOpen(event.cdevice.which);
                }
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (g_controller != nullptr &&
                    event.cdevice.which ==
                        SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_controller))) {
                    SDL_GameControllerClose(g_controller);
                    g_controller = nullptr;
                }
                break;
            default:
                break;
        }
    }
}

// N64 controller button bits, as libultra defines them.
enum : uint16_t {
    BTN_A       = 0x8000,
    BTN_B       = 0x4000,
    BTN_Z       = 0x2000,
    BTN_START   = 0x1000,
    BTN_DUP     = 0x0800,
    BTN_DDOWN   = 0x0400,
    BTN_DLEFT   = 0x0200,
    BTN_DRIGHT  = 0x0100,
    BTN_L       = 0x0020,
    BTN_R       = 0x0010,
    BTN_CUP     = 0x0008,
    BTN_CDOWN   = 0x0004,
    BTN_CLEFT   = 0x0002,
    BTN_CRIGHT  = 0x0001,
};

float axis_to_n64(Sint16 value) {
    // The N64 stick reports roughly +/-80 at full deflection rather than the
    // +/-127 an SDL axis suggests, and Wave Race is unusually sensitive to how
    // the stick is scaled -- steering is analogue throughout.
    constexpr float kDeadzone = 0.12f;
    constexpr float kN64Range = 80.0f;

    float normalized = static_cast<float>(value) / 32767.0f;
    if (normalized > -kDeadzone && normalized < kDeadzone) {
        return 0.0f;
    }
    // Rescale so the stick still reaches full range once past the deadzone.
    const float sign = normalized < 0.0f ? -1.0f : 1.0f;
    const float magnitude = (std::abs(normalized) - kDeadzone) / (1.0f - kDeadzone);
    return sign * magnitude * kN64Range;
}

bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num != 0) {
        return false;
    }

    uint16_t pressed = 0;
    float stick_x = 0.0f;
    float stick_y = 0.0f;

    if (g_controller != nullptr) {
        struct { SDL_GameControllerButton sdl; uint16_t n64; } map[] = {
            { SDL_CONTROLLER_BUTTON_A,             BTN_A },
            { SDL_CONTROLLER_BUTTON_X,             BTN_B },
            { SDL_CONTROLLER_BUTTON_START,         BTN_START },
            { SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  BTN_L },
            { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, BTN_R },
            { SDL_CONTROLLER_BUTTON_DPAD_UP,       BTN_DUP },
            { SDL_CONTROLLER_BUTTON_DPAD_DOWN,     BTN_DDOWN },
            { SDL_CONTROLLER_BUTTON_DPAD_LEFT,     BTN_DLEFT },
            { SDL_CONTROLLER_BUTTON_DPAD_RIGHT,    BTN_DRIGHT },
        };
        for (const auto& entry : map) {
            if (SDL_GameControllerGetButton(g_controller, entry.sdl)) {
                pressed |= entry.n64;
            }
        }
        // Z lives on the left trigger; it is a button on the N64.
        if (SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000) {
            pressed |= BTN_Z;
        }
        // The C buttons map to the right stick, which is how every other N64
        // port does it and what players expect.
        const Sint16 cx = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_RIGHTX);
        const Sint16 cy = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_RIGHTY);
        if (cx < -16000) pressed |= BTN_CLEFT;
        if (cx >  16000) pressed |= BTN_CRIGHT;
        if (cy < -16000) pressed |= BTN_CUP;
        if (cy >  16000) pressed |= BTN_CDOWN;

        stick_x = axis_to_n64(SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTX));
        stick_y = -axis_to_n64(SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTY));
    }

    // Keyboard fallback, so the port is testable without a gamepad attached.
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    if (keys != nullptr) {
        if (keys[SDL_SCANCODE_X])      pressed |= BTN_A;
        if (keys[SDL_SCANCODE_C])      pressed |= BTN_B;
        if (keys[SDL_SCANCODE_Z])      pressed |= BTN_Z;
        if (keys[SDL_SCANCODE_RETURN]) pressed |= BTN_START;
        if (keys[SDL_SCANCODE_A])      pressed |= BTN_L;
        if (keys[SDL_SCANCODE_S])      pressed |= BTN_R;
        if (keys[SDL_SCANCODE_LEFT])   stick_x = -80.0f;
        if (keys[SDL_SCANCODE_RIGHT])  stick_x =  80.0f;
        if (keys[SDL_SCANCODE_UP])     stick_y =  80.0f;
        if (keys[SDL_SCANCODE_DOWN])   stick_y = -80.0f;
    }

    *buttons = pressed;
    *x = stick_x;
    *y = stick_y;
    return true;
}

void set_rumble(int controller_num, bool rumble) {
    if (controller_num != 0 || g_controller == nullptr) {
        return;
    }
    // Wave Race predates the Rumble Pak, but the runtime may still ask.
    SDL_GameControllerRumble(g_controller, rumble ? 0xFFFF : 0, rumble ? 0xFFFF : 0,
                             rumble ? 1000 : 0);
}

ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    if (controller_num == 0) {
        return { ultramodern::input::Device::Controller, ultramodern::input::Pak::None };
    }
    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}

// ---------------------------------------------------------------- audio ----
//
// Placeholder. The samples are discarded and the queue always reports itself
// drained, which keeps the game's audio thread running at the right rate
// without producing sound. Real output is phase 05.

uint32_t g_audio_frequency = 32000;

void queue_samples(int16_t* audio_data, size_t sample_count) {
    (void)audio_data;
    (void)sample_count;
}

size_t get_frames_remaining() {
    // Reporting zero tells the game its buffer has drained, so it keeps
    // producing audio at the expected cadence instead of stalling on a queue
    // that never empties.
    return 0;
}

void set_frequency(uint32_t frequency) {
    g_audio_frequency = frequency;
}

// ------------------------------------------------------------------ rsp ----

// librecomp does not ask us to run a task; it asks which recompiled microcode
// function should run it. Graphics tasks never reach here -- ultramodern routes
// those to the renderer -- so what arrives is the audio microcode, aspMain,
// which would have to be recompiled in its own right to work.
//
// Returning nullptr is permitted but makes librecomp print and exit, which
// would stop the port the first time the game submits an audio task, long
// before anything interesting. This stub instead reports the task as finished
// without doing any work, so boot can proceed in silence. That is a stated
// limitation, not a fix: real audio is phase 05.
RspExitReason rsp_stub_ucode(uint8_t* rdram, uint32_t ucode_addr) {
    (void)rdram;
    (void)ucode_addr;
    return RspExitReason::Broke;
}

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    (void)task;
    return rsp_stub_ucode;
}

// --------------------------------------------------------------- events ----

void vi_callback() {
}

void gfx_init_callback() {
}

// -------------------------------------------------------- error handling ----

void message_box(const char* msg) {
    std::fprintf(stderr, "%s\n", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Wave Race 64: Recompiled", msg, g_window);
}

// -------------------------------------------------------------- threads ----

std::string get_game_thread_name(const OSThread* t) {
    // Kept under 16 bytes including the terminator, which is the limit on
    // several platforms.
    char buf[16];
    std::snprintf(buf, sizeof(buf), "wr64_%d", t != nullptr ? t->id : -1);
    return std::string(buf);
}

// ------------------------------------------------------------------ gfx ----

ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_InitSubSystem failed: %s\n", SDL_GetError());
    }
    return nullptr;
}

ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    g_window = SDL_CreateWindow("Wave Race 64: Recompiled",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                640 * 2, 480 * 2,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (g_window == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return {};
    }

#if defined(_WIN32)
    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    if (SDL_GetWindowWMInfo(g_window, &wm_info) != SDL_TRUE) {
        std::fprintf(stderr, "SDL_GetWindowWMInfo failed: %s\n", SDL_GetError());
        return {};
    }
    return ultramodern::renderer::WindowHandle{ wm_info.info.win.window, GetCurrentThreadId() };
#else
    return g_window;
#endif
}

void update_gfx(ultramodern::gfx_callbacks_t::gfx_data_t) {
    // This is librecomp's main loop body, so counting it distinguishes "the
    // loop never ran" from "the loop ran and then something ended it".
    static uint64_t ticks = 0;
    if (ticks == 0 || ticks == 1000 || ticks == 10000) {
        std::fprintf(stderr, "[wr64] main loop tick %llu\n",
                     static_cast<unsigned long long>(ticks));
        std::fflush(stderr);
    }
    ++ticks;
    poll_input();
}

}  // namespace

namespace wr64 {

ultramodern::input::callbacks_t input_callbacks() {
    return { poll_input, get_input, set_rumble, get_connected_device_info };
}

ultramodern::audio_callbacks_t audio_callbacks() {
    return { queue_samples, get_frames_remaining, set_frequency };
}

recomp::rsp::callbacks_t rsp_callbacks() {
    return { get_rsp_microcode };
}

ultramodern::gfx_callbacks_t gfx_callbacks() {
    return { create_gfx, create_window, update_gfx };
}

ultramodern::events::callbacks_t events_callbacks() {
    return { vi_callback, gfx_init_callback };
}

ultramodern::error_handling::callbacks_t error_handling_callbacks() {
    return { message_box };
}

ultramodern::threads::callbacks_t threads_callbacks() {
    return { get_game_thread_name };
}

ultramodern::renderer::callbacks_t renderer_callbacks() {
    ultramodern::renderer::callbacks_t callbacks{};
    callbacks.create_render_context = create_render_context;
    return callbacks;
}

void shutdown_platform() {
    if (g_controller != nullptr) {
        SDL_GameControllerClose(g_controller);
        g_controller = nullptr;
    }
    if (g_window != nullptr) {
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
    }
    SDL_Quit();
}

}  // namespace wr64
