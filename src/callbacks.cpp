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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
#include <ultramodern/config.hpp>
#include <ultramodern/threads.hpp>
#include <librecomp/rsp.hpp>

#include "wr64/audiodiag.h"
#include "wr64/crash_handler.h"
#if WR64_WITH_FRONTEND
#   include "wr64/frontend.h"
#   include <recompinput/input_events.h>
#   include <recompinput/input_state.h>
#   include <recompinput/players.h>
#   include <recompinput/profiles.h>
#   include <recompui/config.h>
#endif
#include "wr64/display.h"
#include "wr64/haptics.h"
#include "wr64/testdrive.h"
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

#if WR64_WITH_FRONTEND
// Finds the first connected SDL game controller, without tracking add/remove
// events ourselves.
//
// With the frontend on, recompinput::handle_events() (below) is the only thing
// draining SDL's event queue -- SDL_PollEvent removes what it returns, so a
// second loop here would never see anything, including CONTROLLERDEVICEADDED
// and CONTROLLERDEVICEREMOVED, which this code used to react to directly.
// SDL_GameControllerOpen on an already-open device returns the existing
// handle rather than opening it again, so rescanning here every poll is cheap
// and correct rather than a workaround.
void refresh_primary_controller() {
    if (g_controller != nullptr && !SDL_GameControllerGetAttached(g_controller)) {
        g_controller = nullptr;
    }
    if (g_controller == nullptr) {
        for (int i = 0; i < SDL_NumJoysticks(); ++i) {
            if (SDL_IsGameController(i)) {
                g_controller = SDL_GameControllerOpen(i);
                if (g_controller != nullptr) {
                    break;
                }
            }
        }
    }
}

// Who is player one, and who is player two.
//
// The frontend's own answer is a modal: it opens, each player presses a button
// on the pad they want, and the assignment is committed. That suits a game where
// which pad is which matters. Here the first pad is player one and the second is
// player two, and until someone had been through that modal nothing was assigned
// at all -- so a pad drove the game, because the port read it directly, while
// rumble did nothing, because rumble goes through the player list.
//
// So the pads are assigned here instead, in the order SDL reports them, whenever
// that set changes: plug one in and it is player one, plug a second in and it is
// player two. Two is the maximum, because the game's is (frontend.cpp). With no
// pad at all the keyboard becomes player one, so the game is still playable.
// tools/patch_recompinput.py adds the call; the modal still wins while it is
// open, for anyone who wants to choose.
void refresh_players() {
    std::vector<SDL_GameController*> connected;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) continue;
        // Opening an already-open device returns the existing handle.
        if (SDL_GameController* pad = SDL_GameControllerOpen(i)) {
            connected.push_back(pad);
        }
    }

    // The first call always assigns, even with nothing connected. Comparing
    // against the last set alone is what broke input entirely on a machine with
    // no pad attached: an empty set matched an empty set, so nothing was ever
    // assigned, player one did not exist, and get_input reported no controller
    // to the game at all -- keyboard included.
    static bool assigned_once = false;
    static std::vector<SDL_GameController*> assigned;
    if (assigned_once && connected == assigned) {
        return;
    }
    assigned_once = true;
    assigned = connected;

    recompinput::players::auto_assign_controllers(connected.data(), connected.size());

    // Both of player one's profiles, because a keyboard that does nothing looks
    // the same whether it is unbound, unassigned, or simply on other keys.
    std::fprintf(stderr, "[wr64] player 1 profiles: controller %d, keyboard %d\n",
                 recompinput::profiles::get_input_profile_for_player(
                     0, recompinput::InputDevice::Controller),
                 recompinput::profiles::get_input_profile_for_player(
                     0, recompinput::InputDevice::Keyboard));
    std::fprintf(stderr, "[wr64] %zu controller%s connected; assigned to %zu player%s\n",
                 connected.size(), connected.size() == 1 ? "" : "s",
                 recompinput::players::get_number_of_assigned_players(),
                 recompinput::players::get_number_of_assigned_players() == 1 ? "" : "s");
    std::fflush(stderr);
}
#endif

void poll_input() {
#if WR64_WITH_FRONTEND
    // recompinput::handle_events() is the library's own polling loop, and it
    // has to be the only thing draining SDL's event queue: a hand-rolled loop
    // here that just forwarded events to recompui skipped bookkeeping later
    // event handling depends on. Concretely, it never registered a connected
    // controller with recompinput's profile system, so
    // profiles::get_input_profile_for_player(0, Controller) kept returning -1
    // for "no profile assigned" -- and the first real button press then
    // indexed a profile vector with that -1. MSVC's hardened STL reports that
    // as "vector subscript out of range" and fails the process immediately,
    // uncatchably, with no stack trace. Keyboard input never does that lookup,
    // which is why only a gamepad triggered it.
    recompinput::handle_events();

    // recompinput::poll_inputs() is what fills its keyboard snapshot from SDL,
    // and like update_rumble it is exposed for the port to call rather than
    // called by the library itself. Without it, InputState.keys stays null and
    // every keyboard binding reads as unpressed -- while a pad keeps working,
    // because the controller path asks the player's own SDL handle directly.
    // The result is a dead keyboard beside a perfect gamepad, with nothing in
    // the settings or the profiles to suggest why.
    recompinput::poll_inputs();

    refresh_primary_controller();
    refresh_players();
#else
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
#endif
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

// The N64 stick reports roughly +/-80 at full deflection rather than the
// +/-127 an SDL axis suggests. Everything below works in that range, because
// it is the range the game's own code and the scripted-input files are written
// in; get_input divides by it on the way out. See the note there -- passing
// this range to the runtime unscaled is what saturates the stick.
constexpr float kN64Range = 80.0f;

float axis_to_n64(Sint16 value) {
    // Wave Race is unusually sensitive to how the stick is scaled -- steering
    // is analogue throughout.
    constexpr float kDeadzone = 0.12f;

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
#if WR64_WITH_FRONTEND
    // Both players' pads are read through the frontend rather than from SDL
    // here, and that is not only about the second player. recompinput owns the
    // remapping and the per-device profiles the controls tab writes, and
    // profiles::get_n64_input is where they are applied: a port that reads SDL
    // buttons itself, as this one used to, silently ignores every rebinding the
    // player has made. It returns the stick already normalized, which is what
    // the runtime wants (see the note further down).
    if (controller_num < 0 || controller_num >= 2) {
        return false;
    }
    // Player one always exists, because a keyboard is always attached: the
    // assignment gives player one the keyboard profile as well as whatever pad
    // it has, and get_n64_input merges the two, so the keys and the pad both
    // play at any moment without either having to be chosen. Player two exists
    // only once a second pad has been plugged in.
    if (controller_num == 1 && !recompinput::players::get_player_is_assigned(1)) {
        return false;
    }

    uint16_t pressed = 0;
    float stick_x = 0.0f;
    float stick_y = 0.0f;

    // While a menu has input, the game gets none. Otherwise the button that
    // closes a menu also reaches the game behind it -- Start to leave the
    // settings would pause the race underneath.
    if (!wr64::frontend::capturing_input()) {
        recompinput::profiles::get_n64_input(controller_num, &pressed, &stick_x, &stick_y);

        // Scripted input is player one's, and is written in the N64's own
        // +/-80 (see src/testdrive.cpp), so it is scaled to match.
        if (controller_num == 0) {
            uint16_t scripted_buttons = 0;
            float scripted_x = 0.0f;
            float scripted_y = 0.0f;
            wr64::input_script_state(&scripted_buttons, &scripted_x, &scripted_y);
            pressed |= scripted_buttons;
            if (scripted_x != 0.0f) { stick_x = scripted_x / kN64Range; }
            if (scripted_y != 0.0f) { stick_y = scripted_y / kN64Range; }
        }
    }

    *buttons = pressed;
    *x = stick_x;
    *y = stick_y;
    return true;
#else
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
        // The C buttons work the camera, which Wave Race uses constantly, so
        // they need to be reachable without a pad. I/J/K/L keeps them under the
        // right hand while the left drives with the arrow keys.
        if (keys[SDL_SCANCODE_I])      pressed |= BTN_CUP;
        if (keys[SDL_SCANCODE_K])      pressed |= BTN_CDOWN;
        if (keys[SDL_SCANCODE_J])      pressed |= BTN_CLEFT;
        if (keys[SDL_SCANCODE_L])      pressed |= BTN_CRIGHT;
        if (keys[SDL_SCANCODE_T])      pressed |= BTN_DUP;
        if (keys[SDL_SCANCODE_G])      pressed |= BTN_DDOWN;
        if (keys[SDL_SCANCODE_F])      pressed |= BTN_DLEFT;
        if (keys[SDL_SCANCODE_H])      pressed |= BTN_DRIGHT;
        if (keys[SDL_SCANCODE_LEFT])   stick_x = -80.0f;
        if (keys[SDL_SCANCODE_RIGHT])  stick_x =  80.0f;
        if (keys[SDL_SCANCODE_UP])     stick_y =  80.0f;
        if (keys[SDL_SCANCODE_DOWN])   stick_y = -80.0f;
    }

    // Scripted input is merged in rather than replacing the pad, so a run can
    // still be nudged by hand while it plays. See src/testdrive.cpp.
    uint16_t scripted_buttons = 0;
    float scripted_x = 0.0f;
    float scripted_y = 0.0f;
    wr64::input_script_state(&scripted_buttons, &scripted_x, &scripted_y);
    pressed |= scripted_buttons;
    if (scripted_x != 0.0f) { stick_x = scripted_x; }
    if (scripted_y != 0.0f) { stick_y = scripted_y; }

    *buttons = pressed;
    // Normalized, not the N64 range. ultramodern::convert_to_n64_range takes
    // this pair, clamps its *magnitude* to 1.0, and then scales by the stick's
    // own radius (about 82) through the octagonal gate. Handing it +/-80
    // therefore clamps every deflection past the deadzone to the maximum: the
    // direction survives, the magnitude does not, and analogue steering
    // becomes eight-way. Divide by the range the code above works in.
    *x = stick_x / kN64Range;
    *y = stick_y / kN64Range;
    return true;
#endif
}

// The runtime's rumble callback. This game never reaches it -- it has no rumble
// code at all, see src/haptics.cpp -- but a port of a game that does should send
// it the same way the feedback below goes, so that one path owns the motor.
void set_rumble(int controller_num, bool rumble) {
#if WR64_WITH_FRONTEND
    recompinput::set_rumble(controller_num, rumble);
#else
    if (controller_num != 0 || g_controller == nullptr) {
        return;
    }
    SDL_GameControllerRumble(g_controller, rumble ? 0xFFFF : 0, rumble ? 0xFFFF : 0,
                             rumble ? 1000 : 0);
#endif
}

ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
#if WR64_WITH_FRONTEND
    // The game asks this to decide which of its four controller ports has
    // something in it, which is how two-player mode becomes available at all.
    // A player slot with a pad -- or the keyboard, when there is no pad -- is a
    // connected controller; the rest are empty. No Pak: the game reads the
    // Controller Pak for its records and has no rumble code of its own.
    if (controller_num == 0 ||
        (controller_num == 1 && recompinput::players::get_player_is_assigned(1))) {
        return { ultramodern::input::Device::Controller, ultramodern::input::Pak::None };
    }
#else
    if (controller_num == 0) {
        return { ultramodern::input::Device::Controller, ultramodern::input::Pak::None };
    }
#endif
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

// The Sound tab's Main Volume, as a percentage.
//
// recompui defines the slider and reads it back, and nothing upstream ever
// applies it: a port that does not multiply its own samples by this has a volume
// control that does nothing at all, which is what this one had. It is written
// from the UI thread when the slider moves and read on the audio thread for
// every buffer, so it is atomic, and it is applied where the samples are already
// being copied for the channel swap -- one multiply per sample, in a loop that
// was running anyway.
std::atomic<int> g_audio_volume{ 100 };

// Whether the window has focus, and whether losing it should silence the game.
// Both are read on the audio thread and written on the main one. The setting is
// separate from the state so that turning it off restores sound immediately
// rather than at the next focus change.
std::atomic<bool> g_window_focused{ true };
std::atomic<bool> g_mute_unfocused{ true };

// The N64 mixes 16-bit stereo, and the game's own sample rate is whatever it
// asks for through set_frequency.
constexpr int kAudioChannels = 2;
constexpr int kBytesPerFrame = kAudioChannels * static_cast<int>(sizeof(int16_t));

// How deep the game is persuaded to keep the queue, in milliseconds, and how
// much the device takes at a time, in frames. These two decide whether the
// output crackles, and both were wrong.
//
// SDL's queued-audio drain asks for a whole device period every time, takes what
// the queue holds, and **zero-fills the rest without waiting**. So the queue has
// to stay at least one period deep at its trough, not on average. Measured with
// WR64_AUDIO_STATS at a 1024-frame period, the queue averaged 469 frames against
// a period of 1024 and touched zero in every single two-second window of a
// ninety-second run: each period came up about nineteen frames short, and SDL
// put a 0.7 ms hole in the output twenty-six times a second, for the whole run.
// That is the crackle, and nothing upstream of SDL was at fault -- a dump of the
// same run is clean.
//
// The game will not correct this by itself. It sizes each buffer from what
// osAiGetLength reports still queued, so it holds the queue at whatever depth it
// is told to, and when the queue runs dry the zero-fill hides the shortfall from
// it: the frames it never delivered get played as silence and it is never asked
// for them again. It has the capacity to do better -- about 441 frames sixty
// times a second where 448 are wanted, with a ceiling near 464.
//
// So the port under-reports the depth by a fixed amount, the game makes that
// much more, and the queue settles that much deeper. The cost is latency equal
// to the headroom, which is why the period is cut at the same time: a shorter
// period needs less headroom to cover it, and a game that takes no timing from
// its own sound can afford the tens of milliseconds either way.
constexpr uint32_t kQueueHeadroomMs = 30;
constexpr int kDevicePeriodFrames = 256;

// Both are overridable, because the next question about either is always "and at
// a different value?", and rebuilding to ask it is a poor trade.
uint32_t queue_headroom_ms() {
    static const uint32_t value = [] {
        const char* env = std::getenv("WR64_AUDIO_HEADROOM_MS");
        if (env == nullptr) return kQueueHeadroomMs;
        const long parsed = std::strtol(env, nullptr, 10);
        return parsed < 0 ? 0u : static_cast<uint32_t>(parsed);
    }();
    return value;
}

int device_period_frames() {
    static const int value = [] {
        const char* env = std::getenv("WR64_AUDIO_PERIOD");
        if (env == nullptr) return kDevicePeriodFrames;
        const long parsed = std::strtol(env, nullptr, 10);
        return parsed < 32 ? kDevicePeriodFrames : static_cast<int>(parsed);
    }();
    return value;
}

// The headroom in frames at the rate now open, worked out once per device open.
uint32_t g_queue_headroom_frames = 0;

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
    // What SDL takes in one pull, and so the depth the queue has to stay above at
    // every moment rather than on average. See kDevicePeriodFrames.
    want.samples = static_cast<Uint16>(device_period_frames());
    want.callback = nullptr;  // queue-driven

    SDL_AudioSpec have{};
    g_audio_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_audio_device == 0) {
        std::fprintf(stderr, "[wr64] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return false;
    }

    g_queue_headroom_frames =
        static_cast<uint32_t>(uint64_t(have.freq) * queue_headroom_ms() / 1000u);

    SDL_PauseAudioDevice(g_audio_device, 0);
    std::fprintf(stderr, "[wr64] audio device open at %d Hz, %d channels,"
                         " %d-frame period, holding %u frames (%u ms) in hand\n",
                 have.freq, have.channels, have.samples,
                 g_queue_headroom_frames, queue_headroom_ms());
    std::fflush(stderr);

    // `have` is not the hardware's spec. With SDL_AUDIO_ALLOW_ANY_CHANGE unset
    // SDL builds a converter and hands back what was asked for, so the line above
    // says nothing about what the machine is actually running at -- and whether
    // SDL is resampling, and across what ratio, is one of the things being
    // measured. SDL_GetDefaultAudioInfo answers it without opening a second
    // device. Only asked for when the statistics are on.
    if (wr64::audiodiag::stats_enabled()) {
        SDL_AudioSpec device_spec{};
        char* device_name = nullptr;
        const bool known = SDL_GetDefaultAudioInfo(&device_name, &device_spec, 0) == 0;
        if (device_name != nullptr) {
            SDL_free(device_name);
        }
        wr64::audiodiag::device_opened(static_cast<uint32_t>(have.freq),
                                       static_cast<uint32_t>(have.samples),
                                       known ? static_cast<uint32_t>(device_spec.freq) : 0u,
                                       known ? static_cast<uint32_t>(device_spec.samples) : 0u);
    }
    else {
        wr64::audiodiag::device_opened(static_cast<uint32_t>(have.freq),
                                       static_cast<uint32_t>(have.samples), 0u, 0u);
    }
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

    // The volume is applied here rather than to the device: SDL's per-device
    // volume does not exist, and mixing through SDL_MixAudioFormat would be a
    // second pass over the same samples. Scaled with integers, and only when it
    // is not full, so the common case is the copy it always was. A percentage of
    // a signed 16-bit sample cannot overflow it.
    int volume = std::clamp(g_audio_volume.load(std::memory_order_relaxed), 0, 100);
    if (g_mute_unfocused.load(std::memory_order_relaxed) &&
        !g_window_focused.load(std::memory_order_relaxed)) {
        volume = 0;
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
    if (volume == 0) {
        std::fill(unswapped.begin(), unswapped.end(), int16_t{ 0 });
    }
    else if (volume < 100) {
        for (int16_t& sample : unswapped) {
            sample = static_cast<int16_t>(static_cast<int32_t>(sample) * volume / 100);
        }
    }

    // Measured before the buffer goes in, so it is the trough: what the device
    // had left to play at the moment the game got round to making more. See
    // include/wr64/audiodiag.h. Both calls are no-ops unless a WR64_AUDIO_*
    // variable is set.
    wr64::audiodiag::queued(
        static_cast<uint32_t>(SDL_GetQueuedAudioSize(g_audio_device) / kBytesPerFrame),
        unswapped.data(), sample_count);

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
    const size_t frames = SDL_GetQueuedAudioSize(g_audio_device) / kBytesPerFrame;
    // The real depth goes to the diagnostics, because that is what is being
    // measured. No-op unless WR64_AUDIO_STATS is set.
    wr64::audiodiag::polled(static_cast<uint32_t>(frames));

    // The game gets a smaller number, makes up the difference, and the queue
    // settles that much deeper. See kQueueHeadroomMs: this one subtraction is the
    // whole mechanism, and it works only because the game sizes every buffer from
    // the answer to this call.
    return frames > g_queue_headroom_frames ? frames - g_queue_headroom_frames : 0;
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

// Runs the microcode with a watchdog on it, from a private copy of its command
// list, with a net under it.
//
// The watchdog. A microcode that spins forever is the worst failure this code
// has: it faults nothing, prints nothing and returns nothing, so the RSP task
// thread simply stops. The game then freezes with no error at all -- the
// scheduler waits for an SP-complete event that will never come, while every
// other thread carries on, so the audio thread keeps building tasks and the
// window keeps swapping the same finished frame. A wrong microcode load address
// produced exactly that during phase 05; the watchdog makes the next one
// announce itself.
//
// The private copy. The game double-buffers its audio command lists on the
// assumption that the RSP is done with a list long before that buffer's turn
// comes round again, two frames later. That holds on hardware, where the task
// takes about a millisecond, and it holds here of an optimized build. It did
// not hold of the Debug build this port ran as through phase 05 and most of
// phase 06: a task then took 5 to 24 ms against a 16.7 ms frame, and about one
// task in a thousand had its list rewritten by the game while the microcode was
// still DMAing it in, 0x140 bytes at a time. The microcode saw a splice of two
// frames' commands, and one splice in particular -- an ENVMIXER stripped of its
// own SETBUFFs, so inheriting the frame-end SAVEBUFF's 0x200-byte count --
// walked the wet-right channel buffer from 0xE40 off the end of DMEM, wrapping
// onto the command jump table at 0x10. That was the "audio frame dropped"
// click phase 05 could characterise but not place. Copying the list before the
// run costs a memcpy of a few KB per frame and makes the outcome independent
// of how late the task runs. The copy lives in the top of the 8 MB the runtime
// reports; this is a 4 MB cartridge that never reads osMemSize, so nothing of
// the game's is there.
//
// The net. librecomp treats a bad dispatch as fatal: it asserts, and the RSP
// task thread dies with it, stopping the game exactly as a hang would. DMEM is
// reloaded from RDRAM before every task, so the damage never outlives the task
// that caused it, and reporting the task complete costs one frame of audio
// rather than the run. With the copy in place it should never fire; if it
// does, bisect_audio_task explains which command did what.

// The command list of the audio task about to run, recorded by
// get_rsp_microcode so a failed task can be dumped afterwards.
uint32_t g_audio_task_data = 0;
uint32_t g_audio_task_size = 0;
uint32_t g_audio_task_original_ptr = 0;   // the game's buffer; g_audio_task_data may point at our copy

const char* rsp_exit_name(RspExitReason r) {
    switch (r) {
    case RspExitReason::Invalid: return "Invalid";
    case RspExitReason::Broke: return "Broke";
    case RspExitReason::ImemOverrun: return "ImemOverrun";
    case RspExitReason::UnhandledJumpTarget: return "UnhandledJumpTarget";
    case RspExitReason::Unsupported: return "Unsupported";
    case RspExitReason::SwapOverlay: return "SwapOverlay";
    case RspExitReason::UnhandledResumeTarget: return "UnhandledResumeTarget";
    }
    return "?";
}

// Dumps the task's audio command list (ABI 1: eight bytes per command, opcode in
// the top byte) so the command that overruns DMEM can be identified from what
// the game asked for rather than from where the store landed.
void dump_audio_task(const uint8_t* rdram, RspExitReason reason) {
    static const char* const kOps[16] = {
        "SPNOOP", "ADPCM", "CLEARBUFF", "ENVMIXER", "LOADBUFF", "RESAMPLE", "SAVEBUFF", "SEGMENT",
        "SETBUFF", "SETVOL", "DMEMMOVE", "LOADADPCM", "MIXER", "INTERLEAVE", "POLEF", "SETLOOP" };
    const uint32_t count = g_audio_task_size / 8;
    std::fprintf(stderr, "[wr64-audio] task failed with %s: %u commands at 0x%08X\n",
                 rsp_exit_name(reason), count, g_audio_task_data);
    for (uint32_t i = 0; i < count && i < 200; ++i) {
        const uint32_t addr = g_audio_task_data + i * 8;
        const uint32_t w0 = *reinterpret_cast<const uint32_t*>(rdram + (addr & 0x00FFFFFFu));
        const uint32_t w1 = *reinterpret_cast<const uint32_t*>(rdram + ((addr + 4) & 0x00FFFFFFu));
        const uint32_t op = w0 >> 24;
        std::fprintf(stderr, "[wr64-audio]  %3u %-10s f=%02X a=%04X  b=%04X c=%04X   (%08X %08X)\n",
                     i, kOps[op & 15], (w0 >> 16) & 0xFF, w0 & 0xFFFF, w1 >> 16, w1 & 0xFFFF, w0, w1);
    }
    std::fflush(stderr);
}

OSTask g_audio_task{};

// Re-runs the failed task with only its first `count` commands, exactly as
// librecomp sets a task up (the OSTask copied to DMEM 0xFC0, the microcode's
// data DMA'd to DMEM 0). DMEM is rebuilt from RDRAM on every run, so the
// corruption a failed run leaves behind does not carry over.
RspExitReason rerun_audio_task(uint8_t* rdram, uint32_t ucode_addr, uint32_t count) {
    OSTask copy = g_audio_task;
    copy.t.data_size = count * 8;
    std::memcpy(&dmem[0xFC0], &copy, sizeof(OSTask));
    dma_rdram_to_dmem(rdram, 0x0000, copy.t.ucode_data, 0xF80 - 1);
    return aspMain_run(rdram, ucode_addr);
}

}  // namespace
namespace {

// Finds the first command that damages DMEM outside the audio buffers: the
// microcode's constant pool (which holds the command jump table at 0x10) and
// the OSTask copy at 0xFC0. Each prefix of the command list is run from a
// fresh DMEM and both regions compared with what was loaded, so the first
// prefix that dirties either ends with the command that did it. A failed
// dispatch, by contrast, happens whenever the next command's table entry
// happens to be the corrupted one, which can be many commands later.
void bisect_audio_task(uint8_t* rdram, uint32_t ucode_addr) {
    const uint32_t total = g_audio_task_size / 8;
    uint8_t pristine[0x40];
    OSTask copy = g_audio_task;
    dma_rdram_to_dmem(rdram, 0x0000, copy.t.ucode_data, 0xF80 - 1);
    std::memcpy(pristine, dmem, sizeof(pristine));

    uint32_t bad = 0;
    const char* what = nullptr;
    bool top_reported = false;
    for (uint32_t n = 1; n <= total && what == nullptr; ++n) {
        const RspExitReason r = rerun_audio_task(rdram, ucode_addr, n);
        copy.t.data_size = n * 8;
        if (!top_reported && std::memcmp(&dmem[0xFC0], &copy, sizeof(OSTask)) != 0) {
            // Report once which bytes of the top of DMEM the microcode itself
            // writes (scratch use is legitimate; only what it says matters).
            top_reported = true;
            const uint8_t* a = &dmem[0xFC0];
            const uint8_t* b = reinterpret_cast<const uint8_t*>(&copy);
            uint32_t lo = 0x40, hi = 0;
            for (uint32_t i = 0; i < 0x40; ++i) {
                if (a[i] != b[i]) { lo = std::min(lo, i); hi = std::max(hi, i); }
            }
            std::fprintf(stderr, "[wr64-audio] (top of DMEM 0x%03X-0x%03X first written after command %u)\n",
                         0xFC0 + lo, 0xFC0 + hi, n - 1);
        }
        // Only the table itself (0x10-0x2F, the sixteen halfword entries
        // "lh $2, 0x10($2)" indexes into) matters for dispatch correctness.
        // The rest of this 0x40-byte window includes DMA chunk-remainder
        // bookkeeping that legitimately varies with how many bytes of the
        // command list get consumed -- comparing the whole window flagged
        // that as "corruption" and pointed at innocent commands.
        if (std::memcmp(&dmem[0x10], &pristine[0x10], 0x20) != 0) {
            what = "the constant pool / jump table at DMEM 0x000-0x040";
            // Byte-swizzled storage (see RSP_MEM_B in rsp.hpp): logical byte N
            // of DMEM lives at raw offset N^3. A plain reinterpret_cast here
            // would compare the wrong bytes; go through the same swizzle the
            // microcode's own loads and stores use.
            std::fprintf(stderr, "[wr64-audio] bytes changed in 0x000-0x040:");
            for (uint32_t i = 0; i < 0x40; ++i) {
                const uint8_t dv = dmem[i ^ 3];
                const uint8_t pv = pristine[i ^ 3];
                if (dv != pv) {
                    std::fprintf(stderr, " [0x%02X] %02X->%02X", i, pv, dv);
                }
            }
            std::fprintf(stderr, "\n");
        }
        else if (r != RspExitReason::Broke) {
            what = "nothing visible, yet the run failed";
        }
        bad = n;
    }
    if (what == nullptr) {
        std::fprintf(stderr, "[wr64-audio] every prefix ran clean on re-run: the failure depends on"
                             " state the first run changed, not on the command list alone\n");
        std::fflush(stderr);
        return;
    }
    std::fprintf(stderr, "[wr64-audio] first prefix to damage %s is %u commands:"
                         " command %u did it. Context:\n", what, bad, bad - 1);
    static const char* const kOps[16] = {
        "SPNOOP", "ADPCM", "CLEARBUFF", "ENVMIXER", "LOADBUFF", "RESAMPLE", "SAVEBUFF", "SEGMENT",
        "SETBUFF", "SETVOL", "DMEMMOVE", "LOADADPCM", "MIXER", "INTERLEAVE", "POLEF", "SETLOOP" };
    const uint32_t from = bad >= 40 ? bad - 40 : 0;
    for (uint32_t i = from; i < bad + 2 && i < total; ++i) {
        const uint32_t addr = g_audio_task_data + i * 8;
        const uint32_t w0 = *reinterpret_cast<const uint32_t*>(rdram + (addr & 0x00FFFFFFu));
        const uint32_t w1 = *reinterpret_cast<const uint32_t*>(rdram + ((addr + 4) & 0x00FFFFFFu));
        std::fprintf(stderr, "[wr64-audio]  %s%3u %-10s f=%02X a=%04X  b=%04X c=%04X   (%08X %08X)\n",
                     i == bad - 1 ? ">" : " ", i, kOps[(w0 >> 24) & 15], (w0 >> 16) & 0xFF, w0 & 0xFFFF,
                     w1 >> 16, w1 & 0xFFFF, w0, w1);
    }
    std::fflush(stderr);
}

// Where the private copy of the command list lives: physical 0x7E0000, in the
// top of the 8 MB the runtime reports. See the comment above.
constexpr uint32_t kCommandListScratch = 0x807E0000u;
constexpr uint32_t kCommandListScratchSize = 0x10000u;

RspExitReason asp_main_watched(uint8_t* rdram, uint32_t ucode_addr) {
    const uint32_t original_base = g_audio_task_original_ptr & 0x00FFFFFFu;

    // Run from a private copy (see above). Both addresses are 8-byte aligned, so
    // a raw copy preserves RDRAM's byte swizzling. The microcode reads the
    // list's address exactly once, from the OSTask librecomp placed at DMEM
    // 0xFC0, so redirecting that word is all it takes. The diagnostics below
    // are pointed at the copy too: it is what actually ran.
    if (g_audio_task_size <= kCommandListScratchSize) {
        std::memcpy(rdram + (kCommandListScratch & 0x00FFFFFFu), rdram + original_base, g_audio_task_size);
        RSP_MEM_W_STORE(0x30, 0xFC0, kCommandListScratch);
        g_audio_task_data = kCommandListScratch;
        g_audio_task.t.data_ptr = kCommandListScratch;
    }

    // Health metric, kept on purpose: did the game rewrite the original list
    // while the task ran? Harmless now, but it is the measurement that found
    // the bug, and a machine slow enough to trip it is worth knowing about.
    static std::vector<uint8_t> before;
    before.assign(rdram + original_base, rdram + original_base + g_audio_task_size);

    wr64::watch_for_hang("the audio microcode", 5);
    const RspExitReason reason = aspMain_run(rdram, ucode_addr);
    wr64::watch_done();

    {
        static uint64_t changed_runs = 0, runs = 0;
        ++runs;
        if (std::memcmp(before.data(), rdram + original_base, g_audio_task_size) != 0) {
            ++changed_runs;
            if (changed_runs <= 3) {
                std::fprintf(stderr, "[wr64] the game rewrote an audio command list while its task was running"
                                     " (%llu of %llu tasks so far). The task ran from its own copy, so nothing was"
                                     " lost, but audio tasks are running more than a frame late on this machine.\n",
                             static_cast<unsigned long long>(changed_runs),
                             static_cast<unsigned long long>(runs));
                std::fflush(stderr);
            }
        }
    }

    if (reason != RspExitReason::Broke) {
        static uint64_t dropped = 0;
        ++dropped;
        if (dropped <= 40 || dropped % 100 == 0) {
            std::fprintf(stderr, "[wr64] audio frame dropped: the microcode did not reach"
                                 " its break (%llu so far). See src/callbacks.cpp.\n",
                         static_cast<unsigned long long>(dropped));
            std::fflush(stderr);
        }
        if (dropped <= 6) {
            std::fprintf(stderr, "[wr64-audio] task failed with %s: %u commands at 0x%08X\n",
                         rsp_exit_name(reason), g_audio_task_size / 8, g_audio_task_data);
            bisect_audio_task(rdram, ucode_addr);
        }
        return RspExitReason::Broke;
    }
    return reason;
}

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    // Match on the microcode's address rather than the task type: type numbers
    // are a game-level convention, while the address is what actually
    // identifies the code about to run.
    const uint32_t ucode = static_cast<uint32_t>(task->t.ucode) & 0x00FFFFFFu;
    if (ucode == (kAspMainTextStart & 0x00FFFFFFu)) {
        g_audio_task_original_ptr = static_cast<uint32_t>(task->t.data_ptr);
        g_audio_task_data = static_cast<uint32_t>(task->t.data_ptr);
        g_audio_task_size = task->t.data_size;
        g_audio_task = *task;
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
    // Size the window, and decide whether it opens fullscreen.
    //
    // Fullscreen is not just a flag here: RT64 derives the aspect ratio it
    // expands the game into from the swap chain's dimensions. A window created
    // at a 4:3 multiple gives it a 4:3 swap chain, and "Expand" then has
    // nothing to expand into -- the game stays pillarboxed however wide the
    // display is. Opening at the display's own size means the swap chain is the
    // display's shape from the first frame.
    //
    // Windowed, the largest whole multiple of 320x240 that fits is used rather
    // than a hardcoded size. A fixed 2x of 640x480 is 1280x960, taller than a
    // 1536x864 laptop panel: Windows then places the window partly off-screen
    // and the game is cropped with no indication anything is wrong.
    const bool fullscreen = ultramodern::renderer::get_graphics_config().wm_option ==
                            ultramodern::renderer::WindowMode::Fullscreen;

    int width = 320 * 4;
    int height = 240 * 4;
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;

    SDL_Rect display{};
    if (fullscreen && SDL_GetDisplayBounds(0, &display) == 0) {
        width = display.w;
        height = display.h;
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        std::fprintf(stderr, "[wr64] window %dx%d fullscreen (the display's own size)\n",
                     width, height);
        std::fflush(stderr);
    }
    else {
        SDL_Rect usable{};
        if (SDL_GetDisplayUsableBounds(0, &usable) == 0) {
            int scale = 4;
            while (scale > 1 && (320 * scale > usable.w || 240 * scale > usable.h)) {
                --scale;
            }
            width = 320 * scale;
            height = 240 * scale;
            std::fprintf(stderr, "[wr64] window %dx%d (%dx upscale; display has %dx%d usable)\n",
                         width, height, scale, usable.w, usable.h);
            std::fflush(stderr);
        }
    }

    g_window = SDL_CreateWindow("Wave Race 64: Recompiled",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                width, height, flags);
    if (g_window == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return {};
    }

#if WR64_WITH_FRONTEND
    // recompui reads the window through a global of its own; publish ours so
    // the UI measures and draws into the same one the game does.
    wr64::frontend::publish_window(g_window);
#endif

    // Present the region the game draws into, not the framebuffer's borders.
    // The renderer does not exist yet; this only sets what it will read.
    wr64::display::crop_to_content();

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
    wr64::poll_game_state();

    // The motor is driven from here, not from poll_input: this is librecomp's
    // main loop body, which is the thread SDL belongs to, while poll_input is
    // also called on the game thread when the game reads its controllers.
    //
    // With the frontend, recompinput owns the motor: it ramps towards full
    // while the effect is asked for and decays afterwards, the way a Rumble Pak
    // behaved, and scales the result by the Rumble Strength slider -- so zero on
    // that slider is off. Without the frontend the same on/off goes to SDL.
    // Alt-tabbing away should not leave the game shouting from behind another
    // window, and it should not leave the pad buzzing either. The state is kept
    // rather than acted on directly, so that the Sound tab's setting can be
    // turned off and take effect at once.
    const bool focused = g_window == nullptr ||
                         (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    g_window_focused.store(focused, std::memory_order_relaxed);
    if (!focused && g_mute_unfocused.load(std::memory_order_relaxed)) {
        wr64::haptics::silence();
    }

    const bool feedback = focused && wr64::haptics::motor_on();
#if WR64_WITH_FRONTEND
    // Said once, because everything between the race and the motor is silent
    // when it fails: no controller, a driver that refuses SDL's rumble call, or
    // the slider at zero all look identical from the sofa.
    if (feedback) {
        static bool announced = false;
        if (!announced) {
            announced = true;
            std::fprintf(stderr, "[wr64] feedback: first pulse (rumble strength %.0f%%)\n",
                         recompui::config::general::get_rumble_strength());
            std::fflush(stderr);
        }
    }
    recompinput::set_rumble(0, feedback);
    recompinput::update_rumble();
#else
    set_rumble(0, feedback);
#endif
}

}  // namespace

namespace wr64 {

void set_mute_when_unfocused(bool mute) {
    g_mute_unfocused.store(mute, std::memory_order_relaxed);
}

void set_audio_volume(double percent) {
    const int clamped = static_cast<int>(std::lround(std::clamp(percent, 0.0, 100.0)));
    const int previous = g_audio_volume.exchange(clamped, std::memory_order_relaxed);
    if (clamped == previous) {
        return;
    }
    // Worth a line, because this setting used to do nothing: anyone who left the
    // slider at zero while it was inert now has a port that is correctly silent,
    // and the log is where that is explained rather than guessed at.
    std::fprintf(stderr, "[wr64] main volume: %d%%%s\n", clamped,
                 clamped == 0 ? " -- the game will be silent until the Sound tab's"
                                " Main Volume is raised" : "");
    std::fflush(stderr);
}

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
