#include "wr64/haptics.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace wr64::haptics {
namespace {
// USA Rev A native word-swapped RDRAM. All fixed fields are inside 8 MiB.
int32_t word(const uint8_t* ram, uint32_t address) {
    int32_t v; std::memcpy(&v, ram + (address & 0x7fffff), sizeof(v)); return v;
}
uint16_t half(const uint8_t* ram, uint32_t address) {
    uint16_t v; std::memcpy(&v, ram + ((address & 0x7fffff)^2), sizeof(v)); return v;
}
float real(const uint8_t* ram, uint32_t address) {
    float v; std::memcpy(&v, ram + (address & 0x7fffff), sizeof(v)); return std::isfinite(v) ? v : 0;
}
}
Sample observe(const uint8_t* ram) {
    Sample s;
    if (!ram) return s;
    s.tick = uint32_t(word(ram, 0x80151960));
    s.course = uint32_t(word(ram, 0x800D8170));
    s.dt = float(std::clamp(word(ram, 0x800D461C), 1, 3)) / 60;
    s.paused = half(ram, 0x801CE624) != 0xffff;
    s.slot = word(ram, 0x800D48DC); // Human race slot, not character/AI identity.
    const int riders = word(ram, 0x801982F0);
    const int players = word(ram, 0x800DAB28);
    const int state = word(ram, 0x800DAB24);
    s.active = state >= 0x28 && state <= 0x2C && s.course < 9 &&
        players >= 1 && players <= 2 && riders >= 1 && riders <= 4 && s.slot >= 0 && s.slot < riders;
    if (!s.active) return s;
    const uint32_t p = 0x80192690 + uint32_t(s.slot)*0x1718;
    const uint32_t r = 0x801C2938 + uint32_t(s.slot)*0x378;
    s.retired = word(ram,r+0x2EC) != 0;
    s.finished = word(ram,r+0x2F4) != 0;
    // Finish/retire switches to the postrace camera before task submission.
    // Retain that terminal edge and its brief cue, never postrace propulsion.
    s.active = state == 0x28 || s.finished || s.retired;
    const int phase = word(ram, 0x801CE650);
    s.racing = state == 0x28 && word(ram, 0x801CE648) == 3 && phase >= 2 && phase <= 3;
    s.go = s.racing;
    const int counter = word(ram, 0x80228A90);
    if (state == 0x28 && word(ram, 0x80228D08) == 1 && (phase == 1 || phase == 2) && counter >= 0 && counter <= 60)
        s.countdown = counter > 0 ? (counter-1)/15 : 0;
    // The HUD uses this speed scalar times1.8; vertical velocity remains in
    // original simulation units, just as it is in the native landing sound.
    s.speed = std::max(0.0f, real(ram, p+0xB90)*1.8f);
    s.vertical_speed = real(ram, p+0xB7C);
    s.throttle = (half(ram, p+0xC66)&0xA000) ? 1.0f : 0.0f;
    s.steering = std::clamp(float(word(ram,p+0xB60))/80, -1.0f, 1.0f);
    s.airborne = half(ram, p+0xC7C) == 0;
    const int contacts = std::clamp(word(ram,p+0x28)+1, 1, 32);
    s.wet = std::clamp(float(word(ram,p+0xC78))/contacts, 0.0f, 1.0f);
    // Craft/barrier solvers retain their positive closing-speed maxima for
    // this step. Ordinary ramp contact is not mislabeled as a wall collision.
    s.impact = std::max(real(ram,p+0xC40), real(ram,p+0xC44));
    s.collision = s.impact > 2.0f;
    const int animation = word(ram,p+0xC54), animation_frame = word(ram,p+0xC58);
    s.crashed = half(ram,p+0x1608) != 0 || animation == 0x17 || (animation == 7 && animation_frame < 0x38);
    s.lap = word(ram,r);
    s.buoy = word(ram,r+0x0C);
    s.buoy_success = word(ram,r+0x28) == 1;
    s.misses = word(ram,r+0x134);
    s.power = std::clamp(word(ram,r+0x12C), 0, 5);
    return s;
}
}
