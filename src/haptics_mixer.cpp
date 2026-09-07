#include "wr64/haptics_mixer.h"
#include <algorithm>
#include <cmath>

namespace wr64::haptics {
namespace {
float unit(float value) { return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f; }
constexpr double cooldowns[] = {.16, .25, .14, .22, .65, .09, .25, .25, 1.0, 2.0, .25, 1.0, 2.0};
}
const char* event_name(Event event) {
    constexpr const char* names[] = {"wave", "jump", "landing", "collision", "crash", "buoy", "miss", "power", "lap", "finish", "countdown", "go", "retire"};
    return names[static_cast<size_t>(event)];
}
void Mixer::clear_effects() {
    pulses_ = {};
    ambience_ = {};
    air_time_ = descent_ = 0;
    next_pulse_ = 0;
}
void Mixer::reset() {
    clear_effects();
    primed_ = false;
    last_sample_ = last_output_ = -100;
    last_event_.fill(-100);
}
void Mixer::configure(Settings settings) {
    settings.strength = unit(settings.strength);
    settings.ambience = unit(settings.ambience);
    if (static_cast<uint32_t>(settings.mode) > 2) settings.mode = Mode::Off;
    if (settings.mode != settings_.mode || settings.strength == 0) reset();
    settings_ = settings;
}
void Mixer::pulse(double start, double duration, float low, float high) {
    pulses_[next_pulse_++ % pulses_.size()] = {start, duration, unit(low), unit(high)};
}
void Mixer::trigger(Event event, float strength, double now) {
    const auto index = static_cast<size_t>(event);
    if (now - last_event_[index] < cooldowns[index]) return;
    last_event_[index] = now;
    ++counts_[index];
    const float s = unit(strength);
    // Low motor supplies weight; high motor supplies the crisp leading edge.
    // Separate envelopes leave room between cues, instead of stacking a buzz.
    switch (event) {
    case Event::Wave: pulse(now, .095, .18f*s, .22f*s); break;
    case Event::Jump: pulse(now, .075, .08f, .18f); break;
    case Event::Landing:
        pulse(now, .17 + .10*s, .35f + .5f*s, .25f + .28f*s);
        pulse(now+.065, .24, .13f*s, .20f*s); break;
    case Event::Collision:
        pulse(now, .15 + .08*s, .35f + .55f*s, .35f + .35f*s); break;
    case Event::Crash:
        pulse(now, .24, .95f, .75f);
        pulse(now+.18, .18, .45f, .23f); break;
    case Event::Buoy: pulse(now, .065, .10f, .38f); break;
    case Event::Miss:
        pulse(now, .13, .55f, .12f);
        pulse(now+.19, .14, .40f, .08f); break;
    case Event::Power:
        pulse(now, .10, .12f, .34f);
        pulse(now+.12, .15, .26f, .56f); break;
    case Event::Lap:
        pulse(now, .09, .13f, .4f);
        pulse(now+.14, .09, .18f, .5f);
        pulse(now+.28, .17, .32f, .65f); break;
    case Event::Finish:
        pulse(now, .15, .4f, .35f);
        pulse(now+.21, .15, .5f, .55f);
        pulse(now+.43, .34, .65f, .8f); break;
    case Event::Countdown: pulse(now, .07, .12f, .35f); break;
    case Event::Go: pulse(now, .27, .52f, .65f); break;
    case Event::Retire:
        pulse(now, .25, .58f, .12f);
        pulse(now+.31, .27, .28f, .05f); break;
    case Event::Count: break;
    }
}
void Mixer::submit(const Sample& raw, double now) {
    if (!std::isfinite(now)) return;
    Sample s = raw;
    s.speed = std::isfinite(s.speed) ? std::clamp(s.speed, 0.0f, 180.0f) : 0;
    s.vertical_speed = std::isfinite(s.vertical_speed) ? std::clamp(s.vertical_speed, -1000.0f, 1000.0f) : 0;
    s.wet = unit(s.wet); s.throttle = unit(s.throttle);
    s.steering = std::isfinite(s.steering) ? std::clamp(s.steering, -1.0f, 1.0f) : 0;
    s.impact = std::isfinite(s.impact) ? std::max(0.0f,s.impact) : 0;
    s.dt = std::isfinite(s.dt) ? std::clamp(s.dt, .01f, .10f) : 1.0f/30;
    if (!progress_seen_ || s.tick != progress_tick_ || s.course != progress_course_ || s.slot != progress_slot_) {
        progress_seen_ = true; progress_tick_ = s.tick; progress_course_ = s.course;
        progress_slot_ = s.slot; progress_time_ = now;
    } else if (now-progress_time_ > .20) {
        reset(); previous_ = s; return;
    }
    if (!s.active || s.paused || suspended_ || settings_.mode == Mode::Off || settings_.strength == 0) {
        reset(); previous_ = s; last_sample_ = now; return;
    }
    if (!primed_ || s.discontinuity || s.course != previous_.course || s.slot != previous_.slot ||
        s.tick < previous_.tick || s.tick - previous_.tick > 6 || now - last_sample_ > .25) {
        reset(); primed_ = true; previous_ = s; last_sample_ = now; return;
    }
    if (s.tick == previous_.tick) return;
    if (previous_.crashed && !s.crashed) {
        // Recovery can reposition the craft. It is not a new landing or hit.
        clear_effects(); previous_ = s; last_sample_ = now; return;
    }
    const auto& old = previous_;
    if (s.countdown > 0 && s.countdown != old.countdown && !s.racing) trigger(Event::Countdown, 1, now);
    if (s.go && !old.go) trigger(Event::Go, 1, now);
    if (s.finished && !old.finished) trigger(Event::Finish, 1, now);
    if (s.retired && !old.retired) trigger(Event::Retire, 1, now);
    if (s.racing && !s.finished && !s.retired) {
        const bool miss = s.misses > old.misses;
        if (miss) trigger(Event::Miss, 1, now);
        else if (s.buoy_success && s.buoy != old.buoy) trigger(Event::Buoy, 1, now);
        if (!miss && s.power > old.power) trigger(Event::Power, 1, now);
        if (s.lap > old.lap && old.lap >= 1) trigger(Event::Lap, 1, now);
        if (s.crashed && !old.crashed) trigger(Event::Crash, 1, now);
        if (!s.crashed) {
            if (s.airborne) {
                air_time_ += s.dt * (s.tick-old.tick);
                descent_ = std::max(descent_, -s.vertical_speed);
                if (!old.airborne && s.speed > 35) trigger(Event::Jump, 1, now);
            } else {
                if (old.airborne && air_time_ >= .065f && s.speed > 15)
                    trigger(Event::Landing, std::clamp(air_time_*.55f + descent_/160.0f, .08f, 1.0f), now);
                else if (s.wet > old.wet+.22f && s.speed > 30)
                    trigger(Event::Wave, unit(s.speed/100.0f) * unit((s.wet-old.wet)*1.5f), now);
                air_time_ = descent_ = 0;
            }
            if (s.collision && !old.collision)
                trigger(Event::Collision, std::max(.1f,s.impact/22.0f), now);
        } else air_time_ = descent_ = 0;
    }
    previous_ = s;
    last_sample_ = now;
}
MotorLevels Mixer::output(double now, bool allowed) {
    if (!std::isfinite(now)) { reset(); return {}; }
    if (!allowed || settings_.mode == Mode::Off || settings_.strength == 0) {
        suspended_ = !allowed;
        reset();
        return {};
    }
    if (suspended_) { suspended_ = false; reset(); return {}; }
    if (!primed_ || !previous_.active || previous_.paused || now < last_sample_ || now-last_sample_>.20) {
        reset(); return {};
    }
    const double dt = std::clamp(now-last_output_, 0.0, .05);
    last_output_ = now;
    phase_ = std::fmod(phase_+dt, 100.0);
    const auto& s = previous_;
    MotorLevels target{};
    if (settings_.mode == Mode::Full && s.racing && !s.crashed && !s.retired && !s.finished && !s.airborne) {
        const float speed = unit(s.speed/110.0f);
        const float throttle = s.throttle;
        const float engine = throttle * (.045f + .19f*speed);
        const float water = speed * speed * s.wet;
        const float chop = .5f + .32f*std::sin(float(phase_)*(16+14*speed)) + .18f*std::sin(float(phase_)*43);
        target.low = (engine*(.84f+.16f*std::sin(float(phase_)*53)) + .22f*water*chop) * settings_.ambience;
        target.high = (.12f*engine + .12f*water*(.35f+.65f*chop) + .09f*water*std::abs(s.steering)) * settings_.ambience;
        target.right_trigger = (.16f*throttle*speed) * settings_.ambience;
        target.left_trigger = .12f*water*std::abs(s.steering) * settings_.ambience;
    }
    const float alpha = float(1-std::exp(-dt/.035));
    ambience_.low += (target.low-ambience_.low)*alpha;
    ambience_.high += (target.high-ambience_.high)*alpha;
    ambience_.left_trigger += (target.left_trigger-ambience_.left_trigger)*alpha;
    ambience_.right_trigger += (target.right_trigger-ambience_.right_trigger)*alpha;
    MotorLevels out = ambience_;
    for (const auto& p : pulses_) {
        const double age = now-p.start;
        if (age < 0 || age >= p.duration || p.duration <= 0) continue;
        // 6ms attack suppresses clicks; quadratic decay keeps impacts punchy.
        const float tail = float(1-age/p.duration);
        const float envelope = float(std::min(age/.006, 1.0))*tail*tail;
        out.low = std::max(out.low, p.low*envelope);
        out.high = std::max(out.high, p.high*envelope);
    }
    out.left_trigger = settings_.triggers ? unit(std::max(out.left_trigger, out.low*.20f)*settings_.strength) : 0;
    out.right_trigger = settings_.triggers ? unit(std::max(out.right_trigger, out.high*.22f)*settings_.strength) : 0;
    out.low = unit(out.low*settings_.strength);
    out.high = unit(out.high*settings_.strength);
    return out;
}
}
