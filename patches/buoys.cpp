// The course buoys -- the yellow and red L and R markers -- and how many of them
// the game can draw at once.
//
// func_8006E674 draws them in two passes. The first marks each buoy visible if it
// is inside the course's draw distance (the number Draw Distance raises, see
// include/wr64/drawdistance.h) and within 60 degrees either side of where the
// camera faces. The second gives every visible buoy, in table order, one of
// **32** matrix slots per view -- `slti $t5, 0x20` at 0x8006F01C -- and stops at
// the 32nd. A buoy past that keeps its visible mark and is still drawn, through
// whichever slot it held on an earlier frame: in the wrong place, or on top of
// another buoy. And the order is the table's, not distance, so a near buoy can
// be the one left out. At the game's own distance 32 is rarely reached; at
// Maximum it is, and distant buoys pop in as nearer ones leave the view.
//
// Two hooks inside the recompiled function (recomp/wr64.toml, [[patches.hook]]):
//
// - before 0x8006EF10, between the passes: when more buoys are visible than the
//   view has slots, the furthest are marked invisible, so the slots go to the
//   nearest and nothing is drawn through a stale slot;
// - before 0x8006F020, the branch on the slot count: the limit becomes 64 in a
//   one-player race.
//
// Why 64, and only with one player. The slots are matrices in the frame's
// segment 5 buffer at 0xA1C0 + view<<11 + slot<<6: 0x800 bytes, 32 matrices, per
// view, with room for two views before the buffer's own data resumes at 0xB1C0
// (the buffer is 0xB2F0 bytes, func_8006A264). A one-player race has one view, so
// the second view's 32 are free and slots 32-63 land in them. Two views need both
// halves. The draw loops read the slot back with lbu, so any count to 255 would
// read correctly; the memory is what limits it.
//
// Nothing else reads the two arrays the hooks change -- the visibility marks at
// 0x801C08C0 and the slot numbers at 0x801C0B80 are used only inside
// func_8006E674 -- so collision, the missed-buoy count and the course logic,
// which read the buoy records at 0x801BB138, are not touched.
//
// WR64_BUOY_ORIGINAL=1 leaves the game's own behaviour. WR64_BUOY_TRACE=1 prints,
// every few seconds, how many buoys each view wanted and where the ones left out
// were.

#include "recomp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t kBuoyCount   = 0x001BC938;  // int, how many records
constexpr uint32_t kBuoyRecords = 0x001BB138;  // 0x18 each: x, y, z, ..., type at +0x10
constexpr uint32_t kVisibility  = 0x001C08C0;  // int16 per buoy: -1 hidden, else fade 0-255
constexpr uint32_t kPlayers     = 0x000DAB28;  // 1 or 2
constexpr uint32_t kView        = 0x000DAB2C;  // the view being drawn
constexpr uint32_t kGfxPool     = 0x001518B8;  // pointer to the frame's graphics pool
constexpr uint32_t kGfxHead     = 0x00151944;  // gDisplayListHead
constexpr uint32_t kRecordSize  = 0x18;
constexpr size_t   kMaxBuoys    = 256;         // room in the record table
constexpr int32_t  kGameSlots   = 32;
constexpr int32_t  kOnePlayerSlots = 64;
constexpr uint32_t kGfxCommands = 0xC00;       // the pool's display list, in commands

// RDRAM keeps each 32-bit word in host order at the word's own offset; a 16-bit
// value lives at offset ^ 2.
int32_t read_i32(const uint8_t* rdram, uint32_t offset) {
    int32_t v;
    std::memcpy(&v, rdram + (offset & 0x7FFFFF), 4);
    return v;
}
float read_f32(const uint8_t* rdram, uint32_t offset) {
    float v;
    std::memcpy(&v, rdram + (offset & 0x7FFFFF), 4);
    return v;
}
int16_t read_i16(const uint8_t* rdram, uint32_t offset) {
    int16_t v;
    std::memcpy(&v, rdram + ((offset & 0x7FFFFF) ^ 2), 2);
    return v;
}
void write_i16(uint8_t* rdram, uint32_t offset, int16_t v) {
    std::memcpy(rdram + ((offset & 0x7FFFFF) ^ 2), &v, 2);
}

bool env_on(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != 0 && std::strcmp(v, "0") != 0;
}

bool original() {
    static const bool on = env_on("WR64_BUOY_ORIGINAL");
    return on;
}

int32_t slot_limit(const uint8_t* rdram) {
    if (original()) return kGameSlots;
    return read_i32(rdram, kPlayers) == 1 && read_i32(rdram, kView) == 0 ? kOnePlayerSlots : kGameSlots;
}

// Measured per call, printed every kReportCalls calls.
struct Trace {
    uint32_t calls = 0;
    uint32_t over32 = 0, over64 = 0;       // calls with more visible than that
    int32_t  most = 0;                     // most visible in one call
    uint64_t dropped = 0;                  // buoys left without a proper slot
    float    nearest_dropped = 1e9f;       // the closest of those, world units
    float    furthest_drawn = 0;
    uint32_t gfx_most = 0;                 // display-list commands used, at this point
};
constexpr uint32_t kReportCalls = 600;

// The most display-list commands a whole frame used, since the last report.
uint32_t& frame_most() {
    static uint32_t most = 0;
    return most;
}

}  // namespace

// Once per finished frame, from vi_swap_buffer_hook: how much of the game's
// display list the whole frame used, since more buoys means more commands in a
// list with a fixed 0xC00. Trace only.
extern "C" void wr64_buoys_frame_end(uint8_t* rdram) {
    static const bool trace = env_on("WR64_BUOY_TRACE");
    if (!trace) return;
    const uint32_t pool = uint32_t(read_i32(rdram, kGfxPool));
    const uint32_t head = uint32_t(read_i32(rdram, kGfxHead));
    if (head >= pool && head - pool < kGfxCommands * 8) frame_most() = std::max(frame_most(), (head - pool) / 8);
}

extern "C" int32_t wr64_buoys_slot_limit(uint8_t* rdram) {
    return slot_limit(rdram);
}

extern "C" void wr64_buoys_before_slots(uint8_t* rdram, recomp_context* ctx) {
    static const bool trace = env_on("WR64_BUOY_TRACE");
    static Trace t;

    const int32_t count = std::clamp(read_i32(rdram, kBuoyCount), 0, int32_t(kMaxBuoys));
    const uint32_t camera = uint32_t(ctx->r22);   // $s6, the view's camera
    const float cx = read_f32(rdram, camera + 0x4C);
    const float cz = read_f32(rdram, camera + 0x54);
    const int32_t limit = slot_limit(rdram);

    struct Seen { float d2; uint16_t index; };
    std::array<Seen, kMaxBuoys> seen;
    int32_t visible = 0;
    for (int32_t i = 0; i < count; ++i) {
        if (read_i16(rdram, kVisibility + 2 * i) < 0) continue;
        const uint32_t r = kBuoyRecords + kRecordSize * i;
        const float dx = read_f32(rdram, r + 0x0) - cx;
        const float dz = read_f32(rdram, r + 0x8) - cz;
        seen[visible++] = { dx * dx + dz * dz, uint16_t(i) };
    }

    // The ones the game will leave without a proper slot: past the limit, in the
    // order the slot loop walks (table order), unless this reorders them.
    float nearest_dropped = 1e9f, furthest_drawn = 0;
    if (visible > limit && !original()) {
        std::nth_element(seen.begin(), seen.begin() + limit, seen.begin() + visible,
                         [](const Seen& a, const Seen& b) { return a.d2 < b.d2; });
        for (int32_t k = limit; k < visible; ++k) {
            write_i16(rdram, kVisibility + 2 * seen[k].index, -1);
            nearest_dropped = std::min(nearest_dropped, seen[k].d2);
        }
        for (int32_t k = 0; k < limit; ++k) furthest_drawn = std::max(furthest_drawn, seen[k].d2);
    }
    else {
        for (int32_t k = 0; k < visible; ++k) {
            if (k >= limit) nearest_dropped = std::min(nearest_dropped, seen[k].d2);
            else furthest_drawn = std::max(furthest_drawn, seen[k].d2);
        }
    }

    if (!trace) return;
    ++t.calls;
    t.most = std::max(t.most, visible);
    t.over32 += visible > 32;
    t.over64 += visible > 64;
    t.dropped += visible > limit ? uint64_t(visible - limit) : 0;
    if (nearest_dropped < 1e9f) t.nearest_dropped = std::min(t.nearest_dropped, std::sqrt(nearest_dropped));
    t.furthest_drawn = std::max(t.furthest_drawn, std::sqrt(furthest_drawn));
    const uint32_t pool = uint32_t(read_i32(rdram, kGfxPool));
    const uint32_t head = uint32_t(read_i32(rdram, uint32_t(ctx->r29) + 0x550));
    if (head >= pool && head - pool < kGfxCommands * 8) t.gfx_most = std::max(t.gfx_most, (head - pool) / 8);
    if (t.calls % kReportCalls == 0) {
        std::fprintf(stderr,
            "[buoys] %u calls: most visible %d, over 32 in %u, over 64 in %u, limit %d; "
            "%llu left out, nearest of them %.0f; furthest drawn %.0f; "
            "display list at most %u of %u commands before the buoys, %u at the end of a frame\n",
            kReportCalls, t.most, t.over32, t.over64, limit,
            static_cast<unsigned long long>(t.dropped),
            t.nearest_dropped < 1e9f ? t.nearest_dropped : 0.0f, t.furthest_drawn,
            t.gfx_most, kGfxCommands, frame_most());
        std::fflush(stderr);
        t = Trace{};
        frame_most() = 0;
    }
}
