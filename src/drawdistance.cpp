// See include/wr64/drawdistance.h for what this is and how it was found.

#include "wr64/drawdistance.h"

#include <atomic>
#include <cstdio>

namespace wr64::drawdistance {
namespace {

// Where the game leaves the address of the struct the drawing code reads, and
// the offset of the buoy limit inside it. Both are Rev A addresses.
//
// The limit is *not* taken from the static table at 0x800D8578, which also holds
// 5000 and looks like the obvious source. Scaling that table changes nothing --
// the value the drawing code reads stays 5000 -- which is how that coincidence
// was ruled out.
constexpr uint32_t kStructPointer = 0x001C0C80;
constexpr uint32_t kBuoyLimit = 0xA4;

constexpr double kMaxMultiplier = 4.0;

std::atomic<double> g_multiplier{ 1.0 };

// The game's own value, so the multiplier always applies to that rather than
// compounding on what was written last.
uint32_t g_struct = 0;
int32_t g_original = 0;
int32_t g_written = 0;

int32_t* limit_of(uint8_t* rdram, uint32_t address) {
    return reinterpret_cast<int32_t*>(rdram + ((address & 0x00FFFFFFu) + kBuoyLimit));
}

}  // namespace

void set_multiplier(double value) {
    const double clamped = value < 1.0 ? 1.0 : (value > kMaxMultiplier ? kMaxMultiplier : value);
    g_multiplier.store(clamped, std::memory_order_relaxed);
    std::fprintf(stderr, "[wr64] object draw distance: x%.2f\n", clamped);
    std::fflush(stderr);
}

void apply(uint8_t* rdram) {
    const double multiplier = g_multiplier.load(std::memory_order_relaxed);
    const uint32_t address = *reinterpret_cast<const uint32_t*>(rdram + kStructPointer);
    if (address == 0) return;
    int32_t* limit = limit_of(rdram, address);

    if (multiplier <= 1.0) {
        // Put the game's own value back once, then leave the struct alone: at the
        // default setting nothing here writes to memory at all, so the default
        // cannot be the cause of anything.
        if (g_struct != 0) {
            *limit = g_original;
            g_struct = 0;
        }
        return;
    }

    // The struct belongs to the course and the game rewrites it -- when the
    // course changes, and from time to time within one. Either shows as a value
    // that is not what was written into it, and either means the number now there
    // is the game's own and the one to scale.
    if (address != g_struct || *limit != g_written) {
        g_struct = address;
        g_original = *limit;
        std::fprintf(stderr, "[wr64] object draw distance: %d -> %d\n",
                     g_original, int(g_original * multiplier));
        std::fflush(stderr);
    }
    if (g_original <= 0) return;

    g_written = static_cast<int32_t>(g_original * multiplier);
    *limit = g_written;
}

}  // namespace wr64::drawdistance
