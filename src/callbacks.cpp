// Phase 03: the platform callbacks ultramodern asks the project to supply.
//
// ultramodern reimplements libultra but deliberately owns no platform I/O, so
// everything that touches a window, a gamepad, a speaker or an OS dialog is
// provided from here. SDL2 does the work; RT64 already vendors and links it.
//
// Audio and the RSP microcode were honest placeholders through phase 04 --
// neither is needed to reach the boot gate -- and are real as of phase 05:
// samples go to an SDL audio device, and RSP tasks run the audio microcode
// recompiled from the cartridge.

#include "wr64/callbacks.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

#include "wr64/crash_handler.h"
#include "wr64/renderer.h"

// The recompiled audio microcode, produced by RSPRecomp from the cartridge.
//
// Declared at global scope and with C++ linkage, matching how RSPRecomp emits
// it. Inside the anonymous namespace below it would get internal linkage and
// fail to resolve; with extern "C" it would get a different mangled name. Both
// produce the same undefined symbol at link time and neither is obvious.
RspExitReason aspMain_run(uint8_t* rdram, uint32_t ucode_addr);

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
// The RSP's audio microcode writes finished stereo samples into RDRAM and the
// game hands them here; all that is left is to put them on the sound card.
//
// SDL's queue API is used rather than a pull callback because the interface
// ultramodern expects is a queue: the game asks how much is still buffered and
// decides how much more to generate from the answer. Mirroring that directly
// keeps the two in step, where a callback would need its own ring buffer in
// between and a second place for the sample count to drift.

SDL_AudioDeviceID g_audio_device = 0;
uint32_t g_audio_frequency = 32000;

// The N64 mixes 16-bit stereo, and the game's own sample rate is whatever it
// asks for through set_frequency.
constexpr int kAudioChannels = 2;
constexpr int kBytesPerFrame = kAudioChannels * static_cast<int>(sizeof(int16_t));

void close_audio_device() {
    if (g_audio_device != 0) {
        SDL_CloseAudioDevice(g_audio_device);
        g_audio_device = 0;
    }
}

// Opens (or reopens) the output device at the game's current sample rate.
//
// SDL is asked for exactly this format with SDL_AUDIO_ALLOW_ANY_CHANGE unset,
// so it resamples and converts internally if the hardware disagrees. Letting
// SDL change the format instead would mean converting here, and the sample rate
// is the one thing that must not silently differ: the game paces itself against
// how fast the queue drains, so a device running at 48000 while the game
// believes it is feeding 32000 makes the whole audio thread run at the wrong
// speed.
bool open_audio_device() {
    close_audio_device();

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "[wr64] SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_AudioSpec want{};
    want.freq = static_cast<int>(g_audio_frequency);
    want.format = AUDIO_S16SYS;
    want.channels = kAudioChannels;
    // Roughly a 60Hz frame's worth, rounded up to a power of two. Smaller than
    // the game's own buffering, so the queue depth we report back stays
    // dominated by what the game queued rather than by SDL's own latency.
    want.samples = 1024;
    want.callback = nullptr;  // queue-driven

    SDL_AudioSpec have{};
    g_audio_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_audio_device == 0) {
        std::fprintf(stderr, "[wr64] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_PauseAudioDevice(g_audio_device, 0);
    std::fprintf(stderr, "[wr64] audio device open at %d Hz, %d channels\n",
                 have.freq, have.channels);
    std::fflush(stderr);
    return true;
}

void queue_samples(int16_t* audio_data, size_t sample_count) {
    if (g_audio_device == 0) {
        return;
    }

    // sample_count counts individual 16-bit samples, not stereo frames -- see
    // ultramodern::queue_audio_buffer, which divides a byte count by
    // sizeof(int16_t).
    //
    // The two channels arrive swapped, and this is the one place in the project
    // where that is visible. librecomp stores RDRAM byte-swapped so that the
    // MEM_* macros can read big-endian N64 words as native little-endian ones,
    // which it does by XORing the low bits of every address. A pointer handed
    // out raw, as this one is, skips that: within each 32-bit word the two
    // 16-bit halves sit in the opposite order to the cartridge's. Each word
    // holds one left and one right sample, so the audible result is the stereo
    // image mirrored -- correct-sounding music with the channels reversed,
    // which is exactly the kind of bug that survives casual listening.
    // Mirrors the "first display list" log on the graphics side: it separates
    // "the game never asked for audio" from "the audio it asked for is silent",
    // which are different bugs with the same symptom.
    static bool announced_first = false;
    if (!announced_first) {
        announced_first = true;
        std::fprintf(stderr, "[wr64] first audio buffer queued: %zu frames at %u Hz\n",
                     sample_count / 2, g_audio_frequency);
        std::fflush(stderr);
    }

    static std::vector<int16_t> unswapped;
    unswapped.resize(sample_count);
    for (size_t i = 0; i + 1 < sample_count; i += 2) {
        unswapped[i + 0] = audio_data[i + 1];
        unswapped[i + 1] = audio_data[i + 0];
    }
    if (sample_count & 1) {
        unswapped[sample_count - 1] = audio_data[sample_count - 1];
    }

    SDL_QueueAudio(g_audio_device, unswapped.data(),
                   static_cast<Uint32>(sample_count * sizeof(int16_t)));

    // A silent port and a working one queue samples equally often, so the
    // count alone proves nothing; the amplitude is what separates the microcode
    // actually mixing from it dutifully producing zeroes. Reported once, then
    // the scan stops paying for itself -- this runs on the audio thread.
    static bool announced = false;
    if (!announced) {
        for (size_t i = 0; i < sample_count; ++i) {
            if (unswapped[i] != 0) {
                announced = true;
                std::fprintf(stderr, "[wr64] audio is audible (first non-silent buffer"
                                     " after %zu frames)\n", sample_count / 2);
                std::fflush(stderr);
                break;
            }
        }
    }
}

size_t get_frames_remaining() {
    if (g_audio_device == 0) {
        // No device: report the queue permanently drained, so the game keeps
        // generating audio at its normal cadence instead of stalling on a
        // buffer that will never empty.
        return 0;
    }
    return SDL_GetQueuedAudioSize(g_audio_device) / kBytesPerFrame;
}

void set_frequency(uint32_t frequency) {
    if (frequency == g_audio_frequency && g_audio_device != 0) {
        return;
    }
    g_audio_frequency = frequency;
    open_audio_device();
}

// ------------------------------------------------------------------ rsp ----

// librecomp does not ask us to run a task; it asks which recompiled microcode
// function should run it. Graphics tasks never reach here -- ultramodern routes
// those to the renderer -- so what arrives is the audio microcode, aspMain.
//
// Phase 03 answered with a stub that reported every task finished without
// running it, which is why the port was silent. aspMain is now recompiled by
// RSPRecomp from the cartridge (see recomp/aspMain.rsp.toml), and this returns
// the real thing.

// Where aspMain's text sits in RDRAM, from the ELF symbol aspMainTextStart.
constexpr uint32_t kAspMainTextStart = 0x800D37B0;

// Retained for tasks that are not aspMain: reporting the task complete keeps
// the game running, where returning nullptr would make librecomp print and
// exit. Anything landing here is logged once, because a task we cannot run is
// worth knowing about rather than silently dropping.
RspExitReason rsp_unknown_ucode(uint8_t* rdram, uint32_t ucode_addr) {
    (void)rdram;
    static bool reported = false;
    if (!reported) {
        reported = true;
        std::fprintf(stderr, "[wr64] unrecognised RSP microcode at 0x%08X; "
                             "reporting its tasks complete without running them\n",
                     ucode_addr);
        std::fflush(stderr);
    }
    return RspExitReason::Broke;
}

// Runs the microcode with a watchdog on it.
//
// A microcode that spins forever is the worst failure this code has: it faults
// nothing, prints nothing and returns nothing, so the RSP task thread simply
// stops. The game then freezes with no error at all -- the scheduler waits for
// an SP-complete event that will never come, while every other thread carries
// on, so the audio thread keeps building tasks and the window keeps swapping
// the same finished frame. A wrong microcode load address produced exactly that
// during phase 05; this makes the next one announce itself.
RspExitReason asp_main_watched(uint8_t* rdram, uint32_t ucode_addr) {
    wr64::watch_for_hang("the audio microcode", 5);
    const RspExitReason reason = aspMain_run(rdram, ucode_addr);
    wr64::watch_done();
    return reason;
}

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    // Match on the microcode's address rather than the task type: type numbers
    // are a game-level convention, while the address is what actually
    // identifies the code about to run.
    const uint32_t ucode = static_cast<uint32_t>(task->t.ucode) & 0x00FFFFFFu;
    if (ucode == (kAspMainTextStart & 0x00FFFFFFu)) {
        return asp_main_watched;
    }
    static uint64_t others = 0;
    if (++others <= 5) {
        std::fprintf(stderr, "[wr64] non-audio RSP task: type %u ucode 0x%08X\n",
                     static_cast<unsigned>(task->t.type),
                     static_cast<uint32_t>(task->t.ucode));
        std::fflush(stderr);
    }
    return rsp_unknown_ucode;
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
