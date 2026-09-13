// Every buoy in view, drawn: the game's buoy slots, moved out of its matrix
// buffer and uncapped, whenever Draw Distance is above Original.
//
// func_8006E674 draws two tables of buoys, each in two passes -- mark the
// visible ones, then give each visible one a matrix slot and draw it through
// that slot:
//
//   | Table | Records | What | Slots a view | Matrices, segment 5 |
//   |---|---|---|---|---|
//   | A | 0x801BB138, 0x18 each, count 0x801BC938 (<= 256) | the small course buoys, the purple edge rows among them | 32 (`slti` at 0x8006F01C) | 0xA1C0 + view<<11 + slot<<6 |
//   | B | 0x801AEE20, 0x104 each, count 0x801BB120 (<= 64) | the racing buoys and gates: the ones with arrows, passed on the left or right | 12 (`slti` at 0x80070108) | 0x95C0 / 0x9BC0 + view*0x300 + slot<<6 |
//
// Slots are handed out in table order and the loop stops at the cap. A buoy past
// it keeps its visible mark and is drawn through the slot byte it held on an
// earlier frame -- on top of another buoy, or where it used to be -- and the
// order is the table's, not distance. At the game's own distance the caps are
// rarely reached. At a raised one they are, everywhere: up to 120 of table A in
// view at once, the nearest left out 2,032 units away. Near the end of a lap it
// is worst, because the end of the edge row, the start of it past the line and
// the first records of the table are all in view, and the nearest come last.
//
// The caps are the game's buffer being full, not a design choice: segment 5 is
// 0xB2F0 bytes, and table B's slots run straight into table A's, which run into
// the buffer's own data at 0xB1C0. The game uses 4 MB of the 8 the runtime
// provides, so with Draw Distance above Original this puts the matrices in a
// block of its own and removes the caps:
//
// - **A slot is the buoy's record index**, not the next free slot. The address
//   of its matrix is then the same every frame it is drawn, so nothing shifts
//   when a buoy earlier in the table enters or leaves the view, and the renderer
//   pairs each buoy with itself.
// - **The view cone follows the field of view.** The game keeps buoys within 60
//   degrees of the camera's heading, which the Field of View setting and a
//   widescreen window both exceed; a buoy at the edge of the screen was culled
//   while in sight.
// - **The display list is the one real limit left**: 0xC00 commands a frame,
//   eight a buoy. If the buoys in view would not fit with room to spare for the
//   rest of the frame, the furthest of table A are dropped. Measured with every
//   buoy drawn, whole frames used at most 1,723 commands in one player and 1,748
//   in two, and nothing was dropped; this guard is for courses and views that
//   have not been measured.
//
// At Original every hook returns the game's own values, so the frame is exactly
// the game's.
//
// Hooks: recomp/wr64.toml, [[patches.hook]] on func_8006E674, one per place the
// game computes a slot's address, stores a slot byte, compares against a cap or
// loads the cone's threshold -- listed there with the instruction each follows.
// Nothing outside func_8006E674 reads the slot bytes or these matrices; the
// visibility marks of table B are also read by func_8006B334 (the arrows above
// the buoys), which this does not change except where the cone admits more.
//
// WR64_BUOY_ORIGINAL=1 leaves the game's behaviour at every Draw Distance.
// WR64_BUOY_TRACE=1 prints, every 600 calls, what was in view, what was drawn
// and how full the display list got.

#include "recomp.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "wr64/display.h"
#include "wr64/dlrewrite.h"
#include "wr64/drawdistance.h"

namespace {

// The game's.
constexpr uint32_t kCountA       = 0x001BC938;  // int
constexpr uint32_t kRecordsA     = 0x001BB138;  // 0x18 each: x +0, z +8
constexpr uint32_t kVisibleA     = 0x001C08C0;  // int16: -1 hidden, else a fade
constexpr uint32_t kCountB       = 0x001BB120;  // int
constexpr uint32_t kVisibleB     = 0x001C0840;  // int16
constexpr uint32_t kPlayers      = 0x000DAB28;  // 1 or 2
constexpr uint32_t kView         = 0x000DAB2C;  // the view being drawn
constexpr uint32_t kSegment5     = 0x001AE948;  // pointer to this frame's segment 5 buffer
constexpr uint32_t kSegment5Pool = 0x80198368u; // the first of the two buffers
constexpr uint32_t kSegment5Size = 0xB2F0u;
constexpr uint32_t kGfxPool      = 0x001518B8;  // pointer to the frame's graphics pool
constexpr uint32_t kGfxHead      = 0x00151944;  // gDisplayListHead
constexpr uint32_t kGfxCommands  = 0xC00;
constexpr int32_t  kMaxA = 256, kMaxB = 64;

// The port's: a block of the upper 4 MB, clear of the display-list, matrix and
// vertex scratch at 0x700000-0x748FFF (src/dlrewrite.cpp) and the audio command
// list at 0x7E0000 (src/callbacks.cpp). Two buffers, mirroring the game's
// alternation, each holding both views:
//
//   A: 0x750000 + buffer*0x8000 + view*0x4000 + index<<6        256 x 64 bytes a view
//   B: 0x760000 + buffer*0x4000 + view*0x2000 + set*0x1000 + index<<6    64 x 64 a set
constexpr uint32_t kBlockA = 0x00750000u;
constexpr uint32_t kBlockB = 0x00760000u;

// Commands the rest of a frame needs after the buoys, kept free. Measured: 928
// before them and 1,723 at the end of a frame with 64, so under 300 after them
// at the most; this is more than twice that.
constexpr uint32_t kReserve = 700;
constexpr uint32_t kCommandsA = 8;              // per table A buoy
constexpr uint32_t kCommandsB = 15;             // per table B buoy, with its arrow

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
void write_u8(uint8_t* rdram, uint32_t offset, uint8_t v) {
    rdram[(offset & 0x7FFFFF) ^ 3] = v;
}

bool env_on(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != 0 && std::strcmp(v, "0") != 0;
}

// Decided once per call of func_8006E674, at its start, so that every hook in
// the call agrees even if the setting changes while it runs.
struct Call {
    bool active = false;
    uint32_t buffer = 0, view = 0, players = 1;
    float cone = 0.5f;          // the dot-product threshold, the game's 0.5
};
Call g_call;

// cos of the half-angle a buoy may be off the camera's heading and still be in
// sight, from the field of view and the window's shape, with room for the
// buoy's own size. Past a right angle the cone admits everything in reach.
float cone_threshold() {
    const double tan_half_v = std::tan(22.5 * 3.14159265358979 / 180.0) * double(wr64::dlrewrite::fov_scale());
    const double half_h = std::atan(tan_half_v * double(std::max(wr64::display::window_aspect(), 4.0f / 3.0f)));
    const double margin = 15.0 * 3.14159265358979 / 180.0;
    // Never narrower than the game's own 60 degrees: at the game's field of view
    // the screen's half-width plus the margin comes to less than that, and a
    // narrower cone culled buoys the game would have drawn.
    return std::min(0.5f, float(std::cos(std::min(half_h + margin, 3.14159265358979))));
}

struct Trace {
    uint32_t calls = 0;
    int32_t  most_a = 0, most_b = 0;
    uint64_t dropped = 0;
    float    nearest_dropped = 1e9f;
    uint32_t gfx_before = 0;
};
constexpr uint32_t kReportCalls = 600;

uint32_t& frame_most() {
    static uint32_t most = 0;
    return most;
}

}  // namespace

extern "C" {

// At the top of func_8006E674.
void wr64_buoys_begin(uint8_t* rdram, recomp_context*) {
    static const bool original = env_on("WR64_BUOY_ORIGINAL");
    Call c;
    c.active = !original && wr64::drawdistance::reach() > 0;
    c.view = uint32_t(std::clamp(read_i32(rdram, kView), 0, 1));
    c.players = uint32_t(read_i32(rdram, kPlayers));
    const uint32_t buffer = uint32_t(read_i32(rdram, kSegment5));
    c.buffer = buffer >= kSegment5Pool + kSegment5Size ? 1 : 0;
    if (c.active) c.cone = cone_threshold();
    g_call = c;
}

// The cone's threshold, before each table's comparison.
float wr64_buoys_cone(float game) {
    return g_call.active ? g_call.cone : game;
}

// Table A, between the passes: keep what the display list has room for.
void wr64_buoys_before_slots(uint8_t* rdram, recomp_context* ctx) {
    static const bool trace = env_on("WR64_BUOY_TRACE");
    static Trace t;

    const int32_t count_a = std::clamp(read_i32(rdram, kCountA), 0, kMaxA);
    const int32_t count_b = std::clamp(read_i32(rdram, kCountB), 0, kMaxB);
    int32_t visible_b = 0;
    for (int32_t i = 0; i < count_b; ++i) visible_b += read_i16(rdram, kVisibleB + 2 * i) >= 0;

    const uint32_t camera = uint32_t(ctx->r22);   // $s6, this view's camera
    const float cx = read_f32(rdram, camera + 0x4C);
    const float cz = read_f32(rdram, camera + 0x54);
    struct Seen { float d2; uint16_t index; };
    std::array<Seen, kMaxA> seen;
    int32_t visible_a = 0;
    for (int32_t i = 0; i < count_a; ++i) {
        if (read_i16(rdram, kVisibleA + 2 * i) < 0) continue;
        const uint32_t r = kRecordsA + 0x18 * i;
        const float dx = read_f32(rdram, r + 0x0) - cx;
        const float dz = read_f32(rdram, r + 0x8) - cz;
        seen[visible_a++] = { dx * dx + dz * dz, uint16_t(i) };
    }

    // How many of table A fit. The head is the game's, on the stack at 0x550.
    const uint32_t pool = uint32_t(read_i32(rdram, kGfxPool));
    const uint32_t head = uint32_t(read_i32(rdram, uint32_t(ctx->r29) + 0x550));
    const uint32_t used = head >= pool && head - pool < kGfxCommands * 8 ? (head - pool) / 8 : kGfxCommands;
    int32_t room = int32_t(kGfxCommands) - int32_t(used) - int32_t(kReserve) - visible_b * int32_t(kCommandsB);
    // The first of two views leaves the second as much as it takes itself.
    if (g_call.players == 2 && g_call.view == 0) room /= 2;
    int32_t limit = g_call.active ? std::clamp(room / int32_t(kCommandsA), 0, kMaxA) : 32;

    float nearest_dropped = 1e9f;
    if (visible_a > limit && g_call.active) {
        std::nth_element(seen.begin(), seen.begin() + limit, seen.begin() + visible_a,
                         [](const Seen& a, const Seen& b) { return a.d2 < b.d2; });
        for (int32_t k = limit; k < visible_a; ++k) {
            write_i16(rdram, kVisibleA + 2 * seen[k].index, -1);
            nearest_dropped = std::min(nearest_dropped, seen[k].d2);
        }
    }

    if (!trace) return;
    ++t.calls;
    t.most_a = std::max(t.most_a, visible_a);
    t.most_b = std::max(t.most_b, visible_b);
    if (visible_a > limit) t.dropped += uint64_t(visible_a - limit);
    if (nearest_dropped < 1e9f) t.nearest_dropped = std::min(t.nearest_dropped, std::sqrt(nearest_dropped));
    t.gfx_before = std::max(t.gfx_before, used);
    if (t.calls % kReportCalls == 0) {
        std::fprintf(stderr,
            "[buoys] %u calls, %s: most in view %d of table A and %d of table B; %llu dropped, nearest %.0f; "
            "cone %.2f; display list %u before the buoys, %u of %u at the end of a frame\n",
            kReportCalls, g_call.active ? "all drawn" : "the game's slots", t.most_a, t.most_b,
            static_cast<unsigned long long>(t.dropped), t.nearest_dropped < 1e9f ? t.nearest_dropped : 0.0f,
            g_call.cone, t.gfx_before, frame_most(), kGfxCommands);
        std::fflush(stderr);
        t = Trace{};
        frame_most() = 0;
    }
}

// The slot caps, just before each loop's branch: $at = slot count < limit.
int32_t wr64_buoys_slot_limit(uint8_t*) {
    return g_call.active ? kMaxA : 32;
}
int32_t wr64_buoys_slot_limit_b(uint8_t*) {
    return g_call.active ? kMaxB : 12;
}

// Where a matrix is written ($a0, KSEG0) and where the draws read it from
// (the base register, a physical address), when active.
uint32_t wr64_buoys_matrix_a(uint32_t game, uint32_t index) {
    if (!g_call.active) return game;
    return 0x80000000u | (kBlockA + g_call.buffer * 0x8000u + g_call.view * 0x4000u + (index << 6));
}
uint32_t wr64_buoys_base_a(uint32_t game) {
    if (!g_call.active) return game;
    return kBlockA + g_call.buffer * 0x8000u + g_call.view * 0x4000u;
}
uint32_t wr64_buoys_matrix_b(uint32_t game, uint32_t index, uint32_t set) {
    if (!g_call.active) return game;
    return 0x80000000u | (kBlockB + g_call.buffer * 0x4000u + g_call.view * 0x2000u + set * 0x1000u + (index << 6));
}
uint32_t wr64_buoys_base_b(uint32_t game, uint32_t set) {
    if (!g_call.active) return game;
    return kBlockB + g_call.buffer * 0x4000u + g_call.view * 0x2000u + set * 0x1000u;
}

// After the game stores a slot byte, when active, make it the record index, so
// the draws find the matrix written at that index.
void wr64_buoys_slot_byte(uint8_t* rdram, uint32_t byte_address, uint32_t index) {
    if (g_call.active) write_u8(rdram, byte_address, uint8_t(index));
}

// Once per finished frame, from vi_swap_buffer_hook: how full the frame's
// display list got. Trace only.
void wr64_buoys_frame_end(uint8_t* rdram) {
    static const bool trace = env_on("WR64_BUOY_TRACE");
    if (!trace) return;
    const uint32_t pool = uint32_t(read_i32(rdram, kGfxPool));
    const uint32_t head = uint32_t(read_i32(rdram, kGfxHead));
    if (head >= pool && head - pool < kGfxCommands * 8) frame_most() = std::max(frame_most(), (head - pool) / 8);
}

}  // extern "C"
