#include "wr64/haptics_output.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace wr64::haptics {
namespace {
std::uint16_t amplitude(float level) {
    if (!std::isfinite(level)) return 0;
    return static_cast<std::uint16_t>(std::lround(std::clamp(level, 0.0f, 1.0f) * 65535.0f));
}

std::int32_t attached_instance(SDL_GameController* controller) {
    if (!controller || !SDL_GameControllerGetAttached(controller)) return -1;
    SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
    return joystick ? SDL_JoystickInstanceID(joystick) : -1;
}

SDL_GameController* attached_controller(std::int32_t instance) {
    if (instance < 0) return nullptr;
    auto* controller = SDL_GameControllerFromInstanceID(instance);
    return controller && SDL_GameControllerGetAttached(controller) ? controller : nullptr;
}
} // namespace

void ControllerOutput::send(SDL_GameController* controller, Connection& connection,
                            bool triggers, std::uint16_t first, std::uint16_t second) {
    bool& active = triggers ? connection.triggers_active : connection.motors_active;
    bool& failed = triggers ? connection.triggers_failed : connection.motors_failed;
    const bool nonzero = first != 0 || second != 0;
    const auto duration = nonzero ? kOutputWatchdogMilliseconds : 0u;
    const int result = triggers
        ? SDL_GameControllerRumbleTriggers(controller, first, second, duration)
        : SDL_GameControllerRumble(controller, first, second, duration);
    if (result == 0) {
        active = nonzero;
        bool& reported = triggers ? connection.triggers_reported : connection.motors_reported;
        if (nonzero && !reported) {
            reported = true;
            std::fprintf(stderr, "[haptics] controller %d: first %s feedback accepted\n",
                         attached_instance(controller), triggers ? "trigger" : "motor");
        }
        return;
    }

    const std::string error = SDL_GetError();
    // A failed refresh must not leave the previous successful effect running.
    // One best-effort stop is sufficient; the channel stays disabled until a
    // new connection rather than retrying an unsupported driver every frame.
    if (active && nonzero) {
        if (triggers) SDL_GameControllerRumbleTriggers(controller, 0, 0, 0);
        else SDL_GameControllerRumble(controller, 0, 0, 0);
    }
    if (!failed) {
        std::fprintf(stderr, "[haptics] controller %d %s output disabled for this connection: %s\n",
                     attached_instance(controller), triggers ? "trigger" : "motor", error.c_str());
    }
    failed = true;
    active = false;
}

void ControllerOutput::stop_connection(std::int32_t instance_id) {
    const auto found = connections_.find(instance_id);
    if (found == connections_.end()) return;
    auto& connection = found->second;
    // Resolve a live handle afresh. The previously borrowed pointer may have
    // been closed by input polling, or the physical controller unplugged.
    if (auto* controller = attached_controller(instance_id)) {
        if (connection.motors_active) send(controller, connection, false, 0, 0);
        if (connection.triggers_active) send(controller, connection, true, 0, 0);
    }
    connection.motors_active = false;
    connection.triggers_active = false;
}

void ControllerOutput::forget_disconnected() {
    for (auto it = connections_.begin(); it != connections_.end();) {
        if (!attached_controller(it->first)) it = connections_.erase(it);
        else ++it;
    }
}

void ControllerOutput::stop(SDL_GameController* controller) {
    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
        connections_.clear();
        current_instance_id_ = -1;
        return;
    }
    stop_connection(current_instance_id_);
    const auto supplied_id = attached_instance(controller);
    if (supplied_id != current_instance_id_) stop_connection(supplied_id);
    current_instance_id_ = -1;
    forget_disconnected();
}

void ControllerOutput::update(SDL_GameController* controller, MotorLevels levels,
                              bool allowed, double now_seconds) {
    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
        connections_.clear();
        current_instance_id_ = -1;
        return;
    }
    const auto instance_id = attached_instance(controller);
    if (instance_id != current_instance_id_) {
        stop_connection(current_instance_id_);
        current_instance_id_ = instance_id;
    }
    forget_disconnected();
    if (instance_id < 0) return;

    auto [it, inserted] = connections_.try_emplace(instance_id);
    auto& connection = it->second;
    if (inserted) {
        connection.motors_supported = SDL_GameControllerHasRumble(controller) == SDL_TRUE;
        connection.triggers_supported = SDL_GameControllerHasRumbleTriggers(controller) == SDL_TRUE;
        const char* name = SDL_GameControllerName(controller);
        std::fprintf(stderr, "[haptics] controller %d (%s): motors %s, trigger vibration %s\n",
                     instance_id, name ? name : "unnamed",
                     connection.motors_supported ? "available" : "unsupported",
                     connection.triggers_supported ? "available" : "unsupported");
    }

    if (!allowed || !std::isfinite(now_seconds)) {
        stop_connection(instance_id);
        return;
    }
    if (connection.has_send_time && now_seconds < connection.last_send_time) {
        // A restarted clock cannot preserve a future refresh deadline while a
        // motor runs. Stop once and establish a new time origin next update.
        stop_connection(instance_id);
        connection.has_send_time = false;
        return;
    }

    const auto low = amplitude(levels.low), high = amplitude(levels.high);
    const auto left = amplitude(levels.left_trigger), right = amplitude(levels.right_trigger);
    // Stops bypass the refresh limit, including one channel going quiet while
    // the other continues. Idle frames never send redundant zero commands.
    if (!low && !high && connection.motors_active) send(controller, connection, false, 0, 0);
    if (!left && !right && connection.triggers_active) send(controller, connection, true, 0, 0);

    const bool motors = (low || high) && connection.motors_supported && !connection.motors_failed;
    const bool triggers = (left || right) && connection.triggers_supported && !connection.triggers_failed;
    if (!motors && !triggers) return;
    if (connection.has_send_time && now_seconds - connection.last_send_time + 1e-12 < kOutputIntervalSeconds) return;

    connection.has_send_time = true;
    connection.last_send_time = now_seconds;
    if (motors) send(controller, connection, false, low, high);
    if (triggers) send(controller, connection, true, left, right);
}
} // namespace wr64::haptics
