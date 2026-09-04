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
//
// The same two-second window carries the second measurement this file exists
// for, phase 07 B's: how well RT64's interpolation is pairing up. RT64 draws
// the frames in between by pairing each object's transform with the previous
// frame's, and an object that finds no pair is drawn at the newer frame and
// held there -- it steps at the game's rate while everything around it glides.
// tools/patch_rt64.py has RT64 count that, and this reports the rate.
//
// Two counts, because the plain one misleads. Most transforms that fail to
// pair are the course's static scenery, whose matrix is the same every frame
// and is often submitted twice, which is what ties a matcher that goes by
// position; an unpaired object that is not moving looks no different for it.
// The second count keeps only those whose matrix appears nowhere in the
// previous frame, and that is the number worth watching.
//
// WR64_PAIRING=1 asks for it; without it nothing is printed and the accessor
// is not called.

#include "recomp.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include <ultramodern/ultramodern.hpp>

extern "C" void osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx);

// Provided by RT64 through tools/patch_rt64.py, so that nothing here needs
// RT64's headers. The totals run for the life of the process; the rates come
// from the difference between two readings.
extern "C" void RT64_GetTransformPairing(unsigned long long* frames, unsigned long long* total,
                                         unsigned long long* unpaired,
                                         unsigned long long* unpaired_moved);

namespace wr64 {

// Reports the pairing over the window that just closed. Called from the same
// place, and on the same schedule, as the frame-rate report.
void report_pairing() {
    static const bool wanted = std::getenv("WR64_PAIRING") != nullptr;
    if (!wanted) {
        return;
    }

    unsigned long long frames = 0, total = 0, unpaired = 0, unpaired_moved = 0;
    RT64_GetTransformPairing(&frames, &total, &unpaired, &unpaired_moved);

    static unsigned long long last_frames = 0, last_total = 0, last_unpaired = 0, last_moved = 0;
    const unsigned long long d_frames = frames - last_frames;
    const unsigned long long d_total = total - last_total;
    const unsigned long long d_unpaired = unpaired - last_unpaired;
    const unsigned long long d_moved = unpaired_moved - last_moved;
    last_frames = frames;
    last_total = total;
    last_unpaired = unpaired;
    last_moved = unpaired_moved;
    if (d_frames == 0) {
        return;
    }

    std::fprintf(stderr,
                 "[wr64] interpolation: %.0f transforms a frame, %.1f unpaired"
                 " (%.1f of them moved or new)\n",
                 double(d_total) / d_frames, double(d_unpaired) / d_frames,
                 double(d_moved) / d_frames);
    std::fflush(stderr);
}

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

    report_pairing();

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
