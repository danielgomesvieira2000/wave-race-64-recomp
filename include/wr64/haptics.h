#pragma once

// Controller feedback the port works out for itself.
//
// Wave Race 64 (USA, Rev A) has no rumble code at all: it shipped a year before
// the Rumble Pak, and `Motor` appears nowhere in its disassembly outside the
// SDK header that declares osMotorStart. There is nothing to pass through, so
// everything here is inferred from the game's own state -- the craft leaving
// the water and landing again, buoys taken and missed, collisions, laps -- read
// out of RDRAM once per game frame and turned into motor pulses.
//
// The two halves run on different threads and must stay that way:
//
//   capture()  on the game thread, from the frame's osViSwapBuffer, where the
//              game's update for that frame has finished and its memory is
//              consistent. It reads RDRAM and never writes it.
//   update()   on the main thread, from the input poll, where SDL lives. It
//              reads no game memory at all.
//
// Between them is a short list of scheduled pulses under a mutex. Nothing here
// advances the game, changes input, or touches rendering.
//
// The motor is driven through recompinput, which owns the ramp-up and fall-off
// that make a modern pad feel like a Rumble Pak, and which scales everything by
// the Rumble Strength slider in the general tab. That path takes a bool, not an
// amplitude -- as the real Pak did, a fixed-speed motor a game switched on and
// off -- so **an effect's strength here is the length of its pulse**: recompinput
// climbs by 0.17 per update while the motor is asked for and decays afterwards,
// so a 30 ms tick is light and a 200 ms thump is heavy.

#include <cstdint>

namespace wr64::haptics {

// One game frame's reading of the race, taken on the game thread. Everything is
// a plain value: nothing here points into RDRAM after capture returns.
struct Sample {
    uint32_t tick = 0;      // the game's own frame counter
    uint32_t course = 0;
    uint32_t state = 0;      // the game's state number; 0x28 is a race
    int32_t slot = -1;      // the human rider's race slot
    bool active = false;    // a race is running and the slot is valid
    bool paused = false;
    bool racing = false;    // past the countdown, before the results
    bool airborne = false;
    bool crashed = false;
    bool collision = false;
    bool finished = false;
    bool retired = false;
    bool buoy_success = false;
    float speed = 0.0f;
    float vertical_speed = 0.0f;
    float wet = 0.0f;       // 0..1, how much of the hull is touching water
    float impact = 0.0f;    // closing speed of the worst contact this frame
    int32_t buoy = 0;
    int32_t misses = 0;
    int32_t power = 0;
    int32_t lap = 0;
    int32_t countdown = -1; // 3, 2, 1, 0 during the start, -1 otherwise
};

// Game thread, once per frame, after the game's own update.
void capture(const uint8_t* rdram);

// Main thread, once per input poll. Returns whether the motor should be running
// now, which the caller hands to recompinput.
bool motor_on();

// Clears everything scheduled. Called when a race ends or the window loses
// focus, so a pulse cannot outlive what caused it.
void silence();

}  // namespace wr64::haptics
