// Controller feedback worked out from the game's state. See wr64/haptics.h for
// why this exists at all and how the two threads divide.
//
// The addresses below were identified by Elliott Tate in pull request #2 of
// this project, whose fork implements a much larger version of this idea; they
// are re-verified here against a scripted race with WR64_HAPTICS_TRACE, and the
// ones this port relies on are recorded in docs/GAME-INTERNALS.md.

#include "wr64/haptics.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace wr64::haptics {
namespace {

using clock = std::chrono::steady_clock;

double now_seconds() {
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

// ---- reading the game's memory --------------------------------------------
//
// RDRAM is eight megabytes and the game's pointers are KSEG0, so the offset is
// the low 23 bits. The runtime stores 32-bit words in host order, which makes a
// word or a float a plain copy at its offset; a 16-bit half sits at its offset
// XOR 2, as everywhere else in this port.

int32_t word(const uint8_t* ram, uint32_t address) {
    int32_t v = 0;
    std::memcpy(&v, ram + (address & 0x7FFFFFu), sizeof(v));
    return v;
}

uint16_t half(const uint8_t* ram, uint32_t address) {
    uint16_t v = 0;
    std::memcpy(&v, ram + ((address & 0x7FFFFFu) ^ 2u), sizeof(v));
    return v;
}

float real(const uint8_t* ram, uint32_t address) {
    float v = 0.0f;
    std::memcpy(&v, ram + (address & 0x7FFFFFu), sizeof(v));
    return std::isfinite(v) ? v : 0.0f;
}

// ---- where the race is, in USA Rev A ---------------------------------------

constexpr uint32_t kTick        = 0x80151960;  // the game's frame counter
constexpr uint32_t kCourse      = 0x800D8170;  // course id, 0-8
constexpr uint32_t kState       = 0x800DAB24;  // game state; 0x28 is a race
constexpr uint32_t kPlayers     = 0x800DAB28;  // 1 or 2
constexpr uint32_t kRiders      = 0x801982F0;  // riders on the course, 1-4
constexpr uint32_t kHumanSlot   = 0x800D48DC;  // which of them the player is
constexpr uint32_t kPauseFlag   = 0x801CE624;  // half; 0xFFFF while not paused
constexpr uint32_t kRacePhase   = 0x801CE650;  // 2 and 3 are the race proper
constexpr uint32_t kRaceMode    = 0x801CE648;  // 3 while a race is under way
constexpr uint32_t kCountdown   = 0x80228A90;  // counts down 60..0 to the start
constexpr uint32_t kCountingDown = 0x80228D08; // 1 while that counter is live

// Per-rider. Two arrays, each indexed by race slot: the craft's simulation
// state, and the rider's standing in the race.
constexpr uint32_t kCraftBase   = 0x80192690;
constexpr uint32_t kCraftStride = 0x1718;
constexpr uint32_t kCraftContacts   = 0x0028;  // hull contact points tested
constexpr uint32_t kCraftSteering   = 0x0B60;  // word, the stick's own +/-80
constexpr uint32_t kCraftVertical   = 0x0B7C;  // float, up is positive
constexpr uint32_t kCraftSpeed      = 0x0B90;  // float; the HUD shows it x1.8
constexpr uint32_t kCraftImpactA    = 0x0C40;  // float, craft-to-craft closing speed
constexpr uint32_t kCraftImpactB    = 0x0C44;  // float, craft-to-barrier
constexpr uint32_t kCraftAnimation  = 0x0C54;
constexpr uint32_t kCraftAnimFrame  = 0x0C58;
constexpr uint32_t kCraftWet        = 0x0C78;  // contact points touching water
constexpr uint32_t kCraftInWater    = 0x0C7C;  // half; zero while airborne
constexpr uint32_t kCraftCrashed    = 0x1608;  // half

constexpr uint32_t kRaceBase   = 0x801C2938;
constexpr uint32_t kRaceStride = 0x0378;
constexpr uint32_t kRaceLap        = 0x0000;
constexpr uint32_t kRaceBuoy       = 0x000C;  // buoys passed
constexpr uint32_t kRaceBuoyOk     = 0x0028;  // 1 when the last one was correct
constexpr uint32_t kRacePower      = 0x012C;  // 0-5
constexpr uint32_t kRaceMisses     = 0x0134;
constexpr uint32_t kRaceRetired    = 0x02EC;
constexpr uint32_t kRaceFinished   = 0x02F4;

Sample observe(const uint8_t* ram) {
    Sample s;
    if (ram == nullptr) return s;

    s.tick = static_cast<uint32_t>(word(ram, kTick));
    s.course = static_cast<uint32_t>(word(ram, kCourse));
    s.state = static_cast<uint32_t>(word(ram, kState));
    s.slot = word(ram, kHumanSlot);
    s.paused = half(ram, kPauseFlag) != 0xFFFF;

    const int32_t state = static_cast<int32_t>(s.state);
    const int32_t players = word(ram, kPlayers);
    const int32_t riders = word(ram, kRiders);

    // Every one of these has to hold before the arrays below are indexed: a
    // slot read during a menu is whatever the last race left behind.
    s.active = state >= 0x28 && state <= 0x2C && s.course < 9 &&
               players >= 1 && players <= 2 && riders >= 1 && riders <= 4 &&
               s.slot >= 0 && s.slot < riders;
    if (!s.active) return s;

    const uint32_t craft = kCraftBase + static_cast<uint32_t>(s.slot) * kCraftStride;
    const uint32_t race = kRaceBase + static_cast<uint32_t>(s.slot) * kRaceStride;

    s.finished = word(ram, race + kRaceFinished) != 0;
    s.retired = word(ram, race + kRaceRetired) != 0;

    const int32_t phase = word(ram, kRacePhase);
    s.racing = state == 0x28 && word(ram, kRaceMode) == 3 && phase >= 2 && phase <= 3;

    if (state == 0x28 && word(ram, kCountingDown) == 1 && (phase == 1 || phase == 2)) {
        const int32_t counter = word(ram, kCountdown);
        if (counter >= 0 && counter <= 60) s.countdown = counter > 0 ? (counter - 1) / 15 : 0;
    }

    s.speed = std::max(0.0f, real(ram, craft + kCraftSpeed) * 1.8f);
    s.vertical_speed = real(ram, craft + kCraftVertical);
    s.airborne = half(ram, craft + kCraftInWater) == 0;

    // How wet the hull is, as a fraction: the game counts contact points that
    // are in the water against the number it tested.
    const int32_t contacts = std::clamp(word(ram, craft + kCraftContacts) + 1, 1, 32);
    s.wet = std::clamp(static_cast<float>(word(ram, craft + kCraftWet)) /
                       static_cast<float>(contacts), 0.0f, 1.0f);

    // The two solvers keep the worst closing speed they resolved this step.
    // Riding up a ramp is not a collision; a threshold keeps them apart.
    s.impact = std::max(real(ram, craft + kCraftImpactA), real(ram, craft + kCraftImpactB));
    s.collision = s.impact > 2.0f;

    const int32_t animation = word(ram, craft + kCraftAnimation);
    const int32_t animation_frame = word(ram, craft + kCraftAnimFrame);
    s.crashed = half(ram, craft + kCraftCrashed) != 0 || animation == 0x17 ||
                (animation == 7 && animation_frame < 0x38);

    s.lap = word(ram, race + kRaceLap);
    s.buoy = word(ram, race + kRaceBuoy);
    s.buoy_success = word(ram, race + kRaceBuoyOk) == 1;
    s.misses = word(ram, race + kRaceMisses);
    s.power = std::clamp(word(ram, race + kRacePower), 0, 5);
    return s;
}

// ---- what the motor is asked to do -----------------------------------------
//
// A pulse is a window of wall-clock time during which the motor is on. Effects
// are one to three of them, and their length is their strength: recompinput
// climbs 0.17 towards full for every update the motor is wanted and decays
// afterwards, so 30 ms is a tick and 200 ms is a thump. Timestamps are taken on
// the game thread and compared on the main one, which is why they are a steady
// clock rather than a frame count.

struct Pulse {
    double begin = 0.0;
    double end = 0.0;
};

constexpr size_t kMaxPulses = 16;

// Every effect's length, and so every effect's strength, is scaled by this.
// The lengths below were written blind and came out light on a real pad: at 75%
// on the slider they read as a buzz rather than a knock, so they are half again
// as long as first written. Raising this is the way to make everything firmer at
// once; a single effect is tuned in fire() instead.
//
// It cannot fix the ceiling, though. recompinput drives **only the pad's
// high-frequency motor** -- it passes zero for the low-frequency one -- and on
// most pads that is the small, light one. Nothing this file does can produce the
// heavy motor's thump while that holds. See docs/PORTING.md, *Rumble for a game
// that has none*.
constexpr double kStrengthScale = 1.5;

std::mutex g_mutex;
Pulse g_pulses[kMaxPulses];
size_t g_next_pulse = 0;

void schedule(double delay_ms, double length_ms) {
    const double base = now_seconds() + delay_ms / 1000.0;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pulses[g_next_pulse] = { base, base + (length_ms * kStrengthScale) / 1000.0 };
    g_next_pulse = (g_next_pulse + 1) % kMaxPulses;
}

// ---- the events ------------------------------------------------------------

enum class Event {
    Landing, Wave, Collision, Crash, Buoy, Miss, Power, Lap, Finish, Retire,
    Countdown, Go, Count
};

const char* event_name(Event e) {
    switch (e) {
        case Event::Landing:   return "landing";
        case Event::Wave:      return "wave";
        case Event::Collision: return "collision";
        case Event::Crash:     return "crash";
        case Event::Buoy:      return "buoy";
        case Event::Miss:      return "miss";
        case Event::Power:     return "power";
        case Event::Lap:       return "lap";
        case Event::Finish:    return "finish";
        case Event::Retire:    return "retire";
        case Event::Countdown: return "countdown";
        case Event::Go:        return "go";
        default:               return "?";
    }
}

std::FILE* g_trace = nullptr;
bool g_trace_tried = false;

void trace_event(Event e, float strength);

// `strength` is 0..1 and becomes the pulse's length between the effect's own
// lightest and heaviest. Events that are always the same weight pass 1.
void fire(Event e, float strength = 1.0f) {
    strength = std::clamp(strength, 0.0f, 1.0f);
    trace_event(e, strength);
    switch (e) {
        // A landing is the one that matters in this game: the slap as the hull
        // meets the water again. Long enough to be felt in the hands rather
        // than heard as a buzz.
        case Event::Landing:   schedule(0, 60.0 + 140.0 * strength); break;
        // Riding through a wave crest at speed. Deliberately slight: it happens
        // constantly, and anything heavier turns into a continuous hum.
        case Event::Wave:      schedule(0, 22.0 + 30.0 * strength); break;
        case Event::Collision: schedule(0, 70.0 + 150.0 * strength); break;
        case Event::Crash:     schedule(0, 260.0); break;
        case Event::Buoy:      schedule(0, 35.0); break;
        // Two heavier beats, so a missed buoy is unmistakably not a taken one.
        case Event::Miss:      schedule(0, 70.0); schedule(130.0, 70.0); break;
        case Event::Power:     schedule(0, 40.0); schedule(85.0, 55.0); break;
        case Event::Lap:       schedule(0, 55.0); schedule(105.0, 75.0); break;
        case Event::Finish:    schedule(0, 70.0); schedule(120.0, 70.0);
                               schedule(240.0, 160.0); break;
        case Event::Retire:    schedule(0, 110.0); schedule(170.0, 110.0); break;
        case Event::Countdown: schedule(0, 40.0); break;
        case Event::Go:        schedule(0, 170.0); break;
        default: break;
    }
}

// ---- the frame-to-frame state ----------------------------------------------

bool g_primed = false;
Sample g_previous;
float g_air_time = 0.0f;      // seconds this jump has lasted
float g_descent = 0.0f;       // fastest downward speed during it
double g_last_wave = 0.0;     // when a wave slap was last allowed

// The game's own frame time, from the divider it counts retraces against: 1, 2
// or 3, so 60, 30 or 20 frames a second. See docs/GAME-INTERNALS.md, video
// timing.
float frame_seconds(const uint8_t* ram) {
    return static_cast<float>(std::clamp(word(ram, 0x800D461C), 1, 3)) / 60.0f;
}

void reset_state() {
    g_primed = false;
    g_air_time = 0.0f;
    g_descent = 0.0f;
}

// ---- the trace -------------------------------------------------------------
//
// WR64_HAPTICS_TRACE names a file and every frame of every race is written to
// it, with the events it produced. This is how the addresses above were checked
// against a real race rather than taken on trust: speed rises under throttle,
// airborne goes true off a crest, lap counts up once a lap, and a miss appears
// where the HUD says MISS.

void open_trace() {
    if (g_trace_tried) return;
    g_trace_tried = true;
    const char* path = std::getenv("WR64_HAPTICS_TRACE");
    if (path == nullptr) return;
    g_trace = std::fopen(path, "w");
    if (g_trace == nullptr) {
        std::fprintf(stderr, "[wr64] haptics trace: cannot write %s\n", path);
        std::fflush(stderr);
        return;
    }
    std::fprintf(g_trace, "tick,state,slot,speed,vertical,airborne,wet,impact,crashed,"
                          "lap,buoy,buoy_ok,misses,power,countdown,racing,events\n");
}

void trace_event(Event e, float strength) {
    if (g_trace == nullptr) return;
    std::fprintf(g_trace, "%s(%.2f) ", event_name(e), strength);
}

}  // namespace

void capture(const uint8_t* rdram) {
    static const bool disabled = std::getenv("WR64_NO_HAPTICS") != nullptr;
    if (disabled) return;
    open_trace();

    const Sample s = observe(rdram);

    if (!s.active || s.paused) {
        if (g_primed) silence();
        reset_state();
        g_previous = s;
        return;
    }

    // A new race, a different rider, or a jump in the game's own counter: there
    // is no previous frame to compare against, so this one only establishes it.
    if (!g_primed || s.course != g_previous.course || s.slot != g_previous.slot ||
        s.tick < g_previous.tick || s.tick - g_previous.tick > 6) {
        silence();
        reset_state();
        g_primed = true;
        g_previous = s;
        return;
    }
    if (s.tick == g_previous.tick) return;   // the same frame, presented twice

    const Sample& old = g_previous;
    const float dt = frame_seconds(rdram) * static_cast<float>(s.tick - old.tick);

    if (g_trace != nullptr) {
        std::fprintf(g_trace, "%u,0x%02X,%d,%.2f,%.2f,%d,%.2f,%.2f,%d,%d,%d,%d,%d,%d,%d,%d,",
                     s.tick, s.state, s.slot, s.speed, s.vertical_speed, s.airborne ? 1 : 0, s.wet,
                     s.impact, s.crashed ? 1 : 0, s.lap, s.buoy, s.buoy_success ? 1 : 0,
                     s.misses, s.power, s.countdown, s.racing ? 1 : 0);
    }

    // Recovery from a crash puts the craft back on the water somewhere else.
    // That is not a landing and not a collision, so the frame it ends on is
    // taken as a fresh start rather than compared with the one before it.
    if (old.crashed && !s.crashed) {
        silence();
        reset_state();
        g_primed = true;
        g_previous = s;
        if (g_trace != nullptr) { std::fprintf(g_trace, "recovered\n"); std::fflush(g_trace); }
        return;
    }

    if (s.countdown > 0 && s.countdown != old.countdown && !s.racing) fire(Event::Countdown);
    // The first racing frame is where the game fills the standings in: power
    // goes from nothing to full, the lap becomes one, the first buoy is set.
    // None of that is something the rider did, so this frame starts the
    // comparison rather than being compared.
    const bool race_began = s.racing && !old.racing;
    if (race_began) fire(Event::Go);
    if (s.finished && !old.finished) fire(Event::Finish);
    if (s.retired && !old.retired) fire(Event::Retire);

    if (s.racing && !race_began && !s.finished && !s.retired) {
        const bool missed = s.misses > old.misses;
        if (missed) fire(Event::Miss);
        else if (s.buoy_success && s.buoy != old.buoy) fire(Event::Buoy);
        if (!missed && s.power > old.power) fire(Event::Power);
        // Entering lap one is not completing a lap.
        if (s.lap > old.lap && old.lap >= 1) fire(Event::Lap);
        if (s.crashed && !old.crashed) fire(Event::Crash);

        if (!s.crashed) {
            if (s.airborne) {
                g_air_time += dt;
                g_descent = std::max(g_descent, -s.vertical_speed);
            }
            else {
                if (old.airborne && g_air_time >= 0.065f && s.speed > 15.0f) {
                    // How hard it lands: how long it was in the air and how
                    // fast it was coming down.
                    fire(Event::Landing,
                         std::clamp(g_air_time * 0.55f + g_descent / 160.0f, 0.08f, 1.0f));
                }
                else if (s.wet > old.wet + 0.22f && s.speed > 30.0f &&
                         now_seconds() - g_last_wave > 0.18) {
                    g_last_wave = now_seconds();
                    fire(Event::Wave, std::clamp((s.wet - old.wet) * 1.5f, 0.0f, 1.0f) *
                                      std::clamp(s.speed / 100.0f, 0.0f, 1.0f));
                }
                g_air_time = 0.0f;
                g_descent = 0.0f;
            }
            if (s.collision && !old.collision) {
                fire(Event::Collision, std::clamp(s.impact / 22.0f, 0.1f, 1.0f));
            }
        }
        else {
            g_air_time = 0.0f;
            g_descent = 0.0f;
        }
    }

    if (g_trace != nullptr) { std::fprintf(g_trace, "\n"); std::fflush(g_trace); }
    g_previous = s;
}

bool motor_on() {
    const double now = now_seconds();
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const Pulse& p : g_pulses) {
        if (now >= p.begin && now < p.end) return true;
    }
    return false;
}

void silence() {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (Pulse& p : g_pulses) p = {};
}

}  // namespace wr64::haptics
