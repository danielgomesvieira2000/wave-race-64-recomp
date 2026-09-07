#pragma once

#include <array>
#include <cstdint>
#include "wr64/haptics_output.h"

namespace wr64::haptics {
enum class Mode : uint32_t { Off, Impacts, Full };
enum class Event : uint8_t {
    Wave, Jump, Landing, Collision, Crash, Buoy, Miss, Power, Lap,
    Finish, Countdown, Go, Retire, Count
};
constexpr size_t event_count = static_cast<size_t>(Event::Count);
const char* event_name(Event event);

struct Settings {
    Mode mode = Mode::Impacts;
    float strength = 0.8f;
    float ambience = 0.35f;
    bool triggers = true;
};

// Immutable observations made on the guest game thread. Never read guest
// memory from the SDL/output thread or advance gameplay to produce feedback.
struct Sample {
    uint32_t tick = 0, course = 0;
    int slot = -1;
    float dt = 1.0f / 30.0f;
    bool active = false, paused = false, racing = false, go = false;
    bool airborne = false, collision = false, crashed = false;
    bool finished = false, retired = false, discontinuity = false, buoy_success = false;
    float speed = 0, vertical_speed = 0, wet = 0, throttle = 0, steering = 0, impact = 0;
    int buoy = 0, misses = 0, power = 0, lap = 0, countdown = -1;
};

class Mixer {
public:
    void configure(Settings settings);
    void reset();
    void submit(const Sample& sample, double now);
    MotorLevels output(double now, bool allowed = true);
    const std::array<uint64_t, event_count>& counts() const { return counts_; }
    const Sample& observation() const { return previous_; }
private:
    struct Pulse { double start = -100, duration = 0; float low = 0, high = 0; };
    void trigger(Event event, float strength, double now);
    void pulse(double start, double duration, float low, float high);
    void clear_effects();
    Settings settings_{};
    Sample previous_{};
    bool primed_ = false, suspended_ = false;
    double last_sample_ = -100, last_output_ = -100, phase_ = 0;
    // Progress survives effect resets: repeated stale submissions cannot
    // restart an expired motor bed without a new native simulation tick.
    bool progress_seen_ = false;
    uint32_t progress_tick_ = 0, progress_course_ = 0;
    int progress_slot_ = -1;
    double progress_time_ = -100;
    float air_time_ = 0, descent_ = 0;
    MotorLevels ambience_{};
    std::array<Pulse, 24> pulses_{};
    size_t next_pulse_ = 0;
    std::array<double, event_count> last_event_{};
    std::array<uint64_t, event_count> counts_{};
};
}
