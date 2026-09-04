// Phase 06: report the game's own frame rate, and what it is being shown at.
//
// The Framerate setting does not touch the game. Wave Race 64 keeps running at
// the rates it chose for itself -- 20 frames per second in the menus and the
// attract demo, 30 in a race, 60 for a moment at boot; it picks by writing a
// divider the video interrupt handler counts against -- and RT64 draws the
// frames in between by interpolating each object's transform from one game
// frame to the next. So there are two rates to know when something looks
// wrong: the rate the game is producing frames at, which says whether the
// port is keeping up with the game, and the rate they are presented at, which
// says whether the setting took.
//
// The first is measured here, at osViSwapBuffer: the game calls it exactly
// once per frame it finishes, so counting calls over a couple of seconds is
// the game's frame rate. The wrapper calls librecomp's implementation and
// reports only when the rounded rate changes, so a race that stays at 30
// prints one line, and one that keeps dropping to 20 says so each time.
//
// It is registered at osViSwapBuffer's cartridge address in src/overlays.cpp,
// after the resident functions, for the same reason the PI DMA hook is.

#include "recomp.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include <ultramodern/ultramodern.hpp>

extern "C" void osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx);

namespace wr64 {

// The divider the game's video-interrupt handler counts retraces against
// before it lets the game thread run a frame: D_800D461C in the decompilation,
// set to 1, 2 or 3 as each game state begins (Main_Thread in src/game/main.c
// reads it; code_4C750.c and codeseg/B97B0.c write it).
constexpr uint32_t kFrameDividerAddress = 0x800D461Cu;

void vi_swap_buffer_hook(uint8_t* rdram, recomp_context* ctx) {
    osViSwapBuffer_recomp(rdram, ctx);

    using clock = std::chrono::steady_clock;
    static clock::time_point window_start = clock::now();
    static uint32_t swaps = 0;
    static int last_reported = -1;

    ++swaps;
    const auto elapsed = clock::now() - window_start;
    if (elapsed < std::chrono::seconds(2)) {
        return;
    }

    const double seconds = std::chrono::duration<double>(elapsed).count();
    const int rate = static_cast<int>(std::lround(swaps / seconds));
    window_start = clock::now();
    swaps = 0;

    // Reported only when it differs from last time. The game's own target goes
    // beside it: the divider it counts retraces against, so 60 / divider is the
    // rate it asked for, and a measured rate below that is the port not keeping
    // up rather than the game's choice. Time Trial, for one, asks for 20 -- the
    // divider is 3 there -- where the championship asks for 30. (A window that
    // straddles a change reads as some other number for one line.)
    if (rate == last_reported) {
        return;
    }
    last_reported = rate;

    const int32_t divider = MEM_W(0, static_cast<gpr>(static_cast<int32_t>(kFrameDividerAddress)));
    const uint32_t presented = ultramodern::get_target_framerate(static_cast<uint32_t>(rate));
    if (divider > 0) {
        std::fprintf(stderr,
                     "[wr64] the game is running at %d frames per second (it asked for %d);"
                     " presenting at %u (display %u Hz)\n",
                     rate, 60 / divider, presented, ultramodern::get_display_refresh_rate());
    }
    else {
        std::fprintf(stderr,
                     "[wr64] the game is running at %d frames per second; presenting at %u"
                     " (display %u Hz)\n",
                     rate, presented, ultramodern::get_display_refresh_rate());
    }
    std::fflush(stderr);
}

}  // namespace wr64
