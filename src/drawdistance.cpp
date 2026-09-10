// See include/wr64/drawdistance.h for what this is and how each limit was found.

#include "wr64/drawdistance.h"

#include <atomic>
#include <cstdio>

namespace wr64::drawdistance {
namespace {

// One thing the game refuses to draw past a certain distance, and where the
// number it compares against lives.
//
// There is no global draw distance in this game to scale -- the far plane is
// already twenty times further out than anything drawn, and each kind of object
// is culled by its own code with its own rule. So this is a list, and it grows as
// each one is found. Adding an entry is the whole job once the number has been
// located; finding it is the work.
struct Limit {
    const char* what;      // for the log, so a run says which ones took effect
    uint32_t pointer;      // RDRAM offset of a word holding the struct's address
    uint32_t offset;       // the field inside that struct
    int32_t original;      // what the game last wrote there
    int32_t written;       // what was last written on top of it
    uint32_t seen;         // the struct address those two refer to
};

// The buoys. func_8006E674 keeps a buoy only if its distance from the camera is
// less than this, and it reads 5000 while the far plane is at 16,192.
//
// Not yet here, and why:
//   - the arrows and signs at the gates, culled at roughly 1,600 by code that has
//     not been found. See docs/GAME-INTERNALS.md, *The gate markers*.
//   - the animated water, which is not culled at all but generated: a fixed
//     500-vertex patch reaching 922 units, whose spacing is computed per frame
//     rather than stored, so there is no number here to scale.
Limit g_limits[] = {
    { "buoys", 0x001C0C80, 0xA4, 0, 0, 0 },
};

constexpr double kMaxMultiplier = 4.0;
std::atomic<double> g_multiplier{ 1.0 };

int32_t* field(uint8_t* rdram, uint32_t address, uint32_t offset) {
    return reinterpret_cast<int32_t*>(rdram + ((address & 0x00FFFFFFu) + offset));
}

}  // namespace

void set_multiplier(double value) {
    const double clamped = value < 1.0 ? 1.0 : (value > kMaxMultiplier ? kMaxMultiplier : value);
    g_multiplier.store(clamped, std::memory_order_relaxed);
    // The originals are deliberately kept. Recapturing here would read back what
    // was written under the previous setting and treat it as the game's own, so
    // moving the setting twice would compound instead of replacing.
    std::fprintf(stderr, "[wr64] draw distance: x%.2f\n", clamped);
    std::fflush(stderr);
}

void apply(uint8_t* rdram) {
    const double multiplier = g_multiplier.load(std::memory_order_relaxed);

    for (Limit& limit : g_limits) {
        const uint32_t address = *reinterpret_cast<const uint32_t*>(rdram + limit.pointer);
        if (address == 0) continue;
        int32_t* value = field(rdram, address, limit.offset);

        if (multiplier <= 1.0) {
            // Put the game's own number back once, then leave it alone: at the
            // default setting nothing here writes to memory at all, so the
            // default cannot be the cause of anything.
            if (limit.seen != 0) {
                *value = limit.original;
                limit.seen = 0;
            }
            continue;
        }

        // The struct belongs to the course and the game rewrites it -- when the
        // course changes, and from time to time within one. Either shows as a
        // value that is not what was written into it, and either means the number
        // now there is the game's own and the one to scale.
        if (address != limit.seen || *value != limit.written) {
            limit.seen = address;
            limit.original = *value;
            std::fprintf(stderr, "[wr64] draw distance: %s %d -> %d\n",
                         limit.what, limit.original, int(limit.original * multiplier));
            std::fflush(stderr);
        }
        if (limit.original <= 0) continue;

        limit.written = static_cast<int32_t>(limit.original * multiplier);
        *value = limit.written;
    }
}

}  // namespace wr64::drawdistance
