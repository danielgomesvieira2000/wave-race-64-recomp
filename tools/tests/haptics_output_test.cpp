#include "wr64/haptics_output.h"
#include <SDL.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using wr64::haptics::ControllerOutput;
using wr64::haptics::MotorLevels;

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(std::string(message) + ": " + SDL_GetError());
}
struct Command { Uint16 first, second; Uint64 ticks; };
struct VirtualPad {
    SDL_GameController* controller = nullptr;
    SDL_JoystickID instance = -1;
    std::vector<Command> motors, triggers;
    bool fail_motors = false, fail_triggers = false;

    static int SDLCALL motor_callback(void* data, Uint16 first, Uint16 second) {
        auto& pad = *static_cast<VirtualPad*>(data);
        pad.motors.push_back({first, second, SDL_GetTicks64()});
        return pad.fail_motors && (first || second) ? SDL_SetError("virtual motor failure") : 0;
    }
    static int SDLCALL trigger_callback(void* data, Uint16 first, Uint16 second) {
        auto& pad = *static_cast<VirtualPad*>(data);
        pad.triggers.push_back({first, second, SDL_GetTicks64()});
        return pad.fail_triggers && (first || second) ? SDL_SetError("virtual trigger failure") : 0;
    }
    VirtualPad(const char* name, bool with_motors = true, bool with_triggers = true) {
        SDL_VirtualJoystickDesc desc{};
        desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
        desc.naxes = SDL_CONTROLLER_AXIS_MAX;
        desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
        desc.name = name;
        desc.userdata = this;
        desc.Rumble = with_motors ? motor_callback : nullptr;
        desc.RumbleTriggers = with_triggers ? trigger_callback : nullptr;
        const int index = SDL_JoystickAttachVirtualEx(&desc);
        require(index >= 0, "attach virtual controller");
        controller = SDL_GameControllerOpen(index);
        require(controller != nullptr, "open virtual controller");
        instance = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
        require(SDL_GameControllerHasRumble(controller) == (with_motors ? SDL_TRUE : SDL_FALSE), "motor capability");
        require(SDL_GameControllerHasRumbleTriggers(controller) == (with_triggers ? SDL_TRUE : SDL_FALSE), "trigger capability");
    }
    void close_handle() {
        if (controller) SDL_GameControllerClose(controller);
        controller = nullptr;
    }
    void detach() {
        for (int i = 0; i < SDL_NumJoysticks(); ++i) {
            if (SDL_JoystickGetDeviceInstanceID(i) == instance) {
                require(SDL_JoystickDetachVirtual(i) == 0, "detach virtual controller");
                break;
            }
        }
    }
    ~VirtualPad() { close_handle(); detach(); }
};

bool zero(const Command& command) { return command.first == 0 && command.second == 0; }

void probe_attached_controllers() {
    const int count = SDL_NumJoysticks();
    std::printf("SDL attached joystick count before virtual tests: %d\n", count);
    for (int i = 0; i < count; ++i) {
        if (!SDL_IsGameController(i)) {
            std::printf("Read-only device %d: %s (not an SDL game controller)\n", i, SDL_JoystickNameForIndex(i));
            continue;
        }
        SDL_GameController* controller = SDL_GameControllerOpen(i);
        if (!controller) continue;
        std::printf("Read-only controller %d: %s; motors=%d triggers=%d\n", i,
                    SDL_GameControllerName(controller), SDL_GameControllerHasRumble(controller),
                    SDL_GameControllerHasRumbleTriggers(controller));
        // No rumble calls are made on a physical controller in this test.
        SDL_GameControllerClose(controller);
    }
}

void levels_and_stops() {
    VirtualPad pad("WR64 virtual levels"); ControllerOutput output;
    output.update(pad.controller, {.25f,.75f,.5f,2.0f}, true, 0);
    require(pad.motors.size() == 1 && pad.triggers.size() == 1, "both independent output groups");
    require(pad.motors.back().first == 16384 && pad.motors.back().second == 49151, "low/high amplitude conversion");
    require(pad.triggers.back().first == 32768 && pad.triggers.back().second == 65535, "trigger channels and upper clamp");
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    output.update(pad.controller, {-1,inf,nan,-inf}, true, .001);
    require(zero(pad.motors.back()) && zero(pad.triggers.back()), "nonfinite/negative levels stop immediately");
    const auto motor_count = pad.motors.size(), trigger_count = pad.triggers.size();
    for (int i=0;i<20;++i) output.update(pad.controller, {}, false, .002+i*.001);
    output.stop(pad.controller); output.stop(nullptr);
    require(pad.motors.size() == motor_count && pad.triggers.size() == trigger_count, "no redundant zeros while muted or stopped");
    output.update(pad.controller, {1,1,1,1}, true, .1);
    output.update(pad.controller, {1,1,1,1}, false, .1001);
    require(zero(pad.motors.back()) && zero(pad.triggers.back()), "mute bypasses refresh limit");
    output.update(pad.controller, {1,1,1,1}, true, .2);
    output.stop(pad.controller);
    require(zero(pad.motors.back()) && zero(pad.triggers.back()), "explicit shutdown stop");
}

void rate_limit_and_independent_zero() {
    VirtualPad pad("WR64 virtual rate"); ControllerOutput output;
    output.update(pad.controller, {.2f,.3f,.4f,.5f}, true, 0);
    for (int i=1;i<8;++i) output.update(pad.controller, {.6f,.7f,.8f,.9f}, true, i*.001);
    require(pad.motors.size() == 1 && pad.triggers.size() == 1, "changes inside8ms are coalesced");
    output.update(pad.controller, {.6f,.7f,.8f,.9f}, true, .008);
    require(pad.motors.size() == 2 && pad.triggers.size() == 2, "125Hz refresh boundary");
    output.update(pad.controller, {.6f,.7f,0,0}, true, .0081);
    require(pad.motors.size() == 2 && zero(pad.triggers.back()), "trigger zero bypasses limit independently");
    output.update(pad.controller, {}, true, .0082);
    require(zero(pad.motors.back()), "motor zero bypasses limit");
    output.stop(pad.controller);
}

void watchdog_and_unchanged_refresh() {
    VirtualPad pad("WR64 virtual watchdog"); ControllerOutput output;
    auto now=[] { return SDL_GetTicks64()/1000.0; };
    output.update(pad.controller, {.4f,.8f,.2f,.3f}, true, now());
    SDL_Delay(30);
    output.update(pad.controller, {.4f,.8f,.2f,.3f}, true, now());
    // SDL intentionally coalesces identical amplitudes while extending their
    // expiration. Observe that extension through real virtual-device stops.
    const auto refreshed = SDL_GetTicks64();
    SDL_Delay(30); SDL_GameControllerUpdate();
    require(pad.motors.size() == 1 && pad.triggers.size() == 1, "unchanged refresh extends50ms watchdog");
    SDL_Delay(35); SDL_GameControllerUpdate();
    require(zero(pad.motors.back()) && zero(pad.triggers.back()), "both groups expire without updates");
    require(pad.motors.back().ticks-refreshed >= 50 && pad.triggers.back().ticks-refreshed >= 50, "short watchdog deadline honored");
    output.stop(pad.controller);
}

void reroute_and_closed_handle() {
    VirtualPad first("WR64 virtual first"), second("WR64 virtual second"); ControllerOutput output;
    output.update(first.controller, {.5f,.6f,.7f,.8f}, true, 0);
    output.update(second.controller, {.1f,.2f,.3f,.4f}, true, .001);
    require(zero(first.motors.back()) && zero(first.triggers.back()), "rerouting stops old still-live pad");
    require(!zero(second.motors.back()) && !zero(second.triggers.back()), "rerouting starts new pad");
    second.close_handle(); // backend retains no pointer that this invalidates
    output.update(nullptr, {}, true, .01);
    output.update(first.controller, {.5f,.6f,.7f,.8f}, true, .02);
    first.detach();
    output.update(first.controller, {.5f,.6f,.7f,.8f}, true, .03);
    output.stop(nullptr);
}

void unsupported_and_failure_cache() {
    VirtualPad unsupported("WR64 virtual unsupported",false,false); ControllerOutput output;
    SDL_ClearError();
    for (int i=0;i<20;++i) output.update(unsupported.controller, {1,1,1,1}, true, i*.01);
    require(std::string(SDL_GetError()).empty(), "unsupported capabilities never invoke rumble driver");
    VirtualPad failing("WR64 virtual failing"), other("WR64 virtual supported");
    failing.fail_motors = true;
    output.update(failing.controller, {.5f,.6f,.7f,.8f}, true, 1);
    require(failing.motors.size() == 1 && failing.triggers.size() == 1, "one failed motor call does not disable triggers");
    for (int i=0;i<10;++i) output.update(failing.controller, {.8f,.9f,.2f+i*.01f,.3f}, true, 1.01+i*.01);
    require(failing.motors.size() == 1 && failing.triggers.size() > 1, "failure cached across updates");
    output.update(other.controller, {.1f,.2f,0,0}, true, 2);
    output.update(failing.controller, {.5f,.6f,.7f,.8f}, true, 3);
    require(failing.motors.size() == 1, "failure cached when rerouting back within same connection");
    output.stop(failing.controller);
    failing.close_handle(); failing.detach();
    VirtualPad reconnected("WR64 virtual reconnected");
    output.update(reconnected.controller, {.5f,.6f,.7f,.8f}, true, 4);
    require(reconnected.motors.size() == 1, "new connection re-probes capabilities and can rumble");
    reconnected.fail_triggers = true;
    output.update(reconnected.controller, {.6f,.7f,.8f,.9f}, true, 4.01);
    require(reconnected.triggers.size() == 3 && zero(reconnected.triggers.back()), "failed active refresh sends one best-effort stop");
    const auto count = reconnected.triggers.size();
    output.update(reconnected.controller, {.7f,.8f,1,1}, true, 4.02);
    require(reconnected.triggers.size() == count, "failed triggers stay off while motors continue");
    output.stop(reconnected.controller);
}

void invalid_time() {
    VirtualPad pad("WR64 virtual time"); ControllerOutput output;
    output.update(pad.controller, {.5f,.6f,.7f,.8f}, true, 1);
    output.update(pad.controller, {.5f,.6f,.7f,.8f}, true, std::numeric_limits<double>::quiet_NaN());
    require(zero(pad.motors.back()) && zero(pad.triggers.back()), "invalid clock stops output");
    output.update(pad.controller, {.5f,.6f,.7f,.8f}, true, 2);
    output.update(pad.controller, {.5f,.6f,.7f,.8f}, true, .1);
    require(zero(pad.motors.back()), "rewinding clock stops once");
    output.update(pad.controller, {.5f,.6f,.7f,.8f}, true, .11);
    require(!zero(pad.motors.back()), "new clock origin recovers");
    output.stop(pad.controller);
}
} // namespace

int main() {
    try {
        require(SDL_Init(SDL_INIT_GAMECONTROLLER|SDL_INIT_JOYSTICK|SDL_INIT_EVENTS|SDL_INIT_TIMER) == 0, "initialize SDL");
        probe_attached_controllers();
        levels_and_stops(); rate_limit_and_independent_zero(); watchdog_and_unchanged_refresh();
        reroute_and_closed_handle(); unsupported_and_failure_cache(); invalid_time();
        SDL_Quit();
        ControllerOutput output; output.update(nullptr, {}, false, 0); output.stop(nullptr);
        std::puts("PASS haptics output: levels,125Hz,50ms watchdog,mute,reroute,disconnect,reconnect,failures,nonfinite");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"FAIL: %s\n",error.what()); SDL_Quit(); return 1;
    }
}
