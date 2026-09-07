#pragma once

#include <cstdint>
#include <unordered_map>

struct _SDL_GameController;
typedef struct _SDL_GameController SDL_GameController;

namespace wr64::haptics {

struct MotorLevels {
    float low = 0;
    float high = 0;
    float left_trigger = 0;
    float right_trigger = 0;
};

inline constexpr double kOutputIntervalSeconds = 1.0 / 125.0;
inline constexpr std::uint32_t kOutputWatchdogMilliseconds = 50;

// Main-thread only. The controller is borrowed for this call and must be a
// currently valid SDL handle, or nullptr. Only its instance ID is retained.
// Call stop() before closing the current handle or shutting down SDL. The
// destructor intentionally makes no SDL calls.
class ControllerOutput {
public:
    void update(SDL_GameController* controller, MotorLevels levels,
                bool allowed, double now_seconds);
    void stop(SDL_GameController* controller);

private:
    struct Connection {
        bool motors_supported = false;
        bool triggers_supported = false;
        bool motors_failed = false;
        bool triggers_failed = false;
        bool motors_active = false;
        bool triggers_active = false;
        bool motors_reported = false;
        bool triggers_reported = false;
        bool has_send_time = false;
        double last_send_time = 0;
    };

    void stop_connection(std::int32_t instance_id);
    void send(SDL_GameController* controller, Connection& connection,
              bool triggers, std::uint16_t first, std::uint16_t second);
    void forget_disconnected();

    std::int32_t current_instance_id_ = -1;
    std::unordered_map<std::int32_t, Connection> connections_;
};

} // namespace wr64::haptics
