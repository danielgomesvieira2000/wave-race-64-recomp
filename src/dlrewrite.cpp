// The display-list rewriter. See include/wr64/dlrewrite.h for what it is for.
//
// What it does today (phase 07, step A1): in a menu state, every stretch of
// the list drawn under a perspective projection is given a viewport whose
// origin is the centre of the frame. RT64 keeps a full-width projection
// "adjusted" to the widened frame by default -- for a perspective projection
// that means a wider field of view, for an orthographic one it means the 2D
// stays at 4:3 in the middle -- and that is right for a race and wrong for
// the watercraft and rider select screens, where the 2D frames stay at 4:3
// while the 3D models drawn into them spread out with the frustum. A viewport
// with an origin switches both adjustments off for that projection: the
// frustum is left as the game set it and rendered into the centred 4:3 region,
// which is exactly where the 2D layout is. The models land in their frames.
//
// RT64 takes the origin from the viewport in force when the projection is
// used, and only a viewport load sets it, so the rewriter inserts the
// alignment and then reissues the game's own last viewport command before the
// perspective projection load, and clears both again before the next
// orthographic one. The alignment carries an offset that cancels the
// displacement RT64 adds for a centred origin, so the game's viewport data
// is used exactly as it is. In a race nothing is inserted at all.
//
// How the list is read. The game builds one top-level list per frame with
// every matrix load inline, and calls its model display lists from it. This
// walks the top-level list only, copying each command, following branches in
// place and leaving calls as calls: the called lists are the game's static
// models, they live where they live, and RT64 runs them from there. Segment
// table writes are tracked as they go by so that a matrix's address can be
// resolved and its data read -- the perspective test is RT64's own: element
// [3][3] is zero for a perspective projection and one for an orthographic
// one.
//
// Memory. RDRAM in the runtime stores 32-bit words natively, so a command is
// two host words at its physical offset, and RT64 reads it the same way. The
// 16-bit halves of a matrix are the exception: they sit at their offset XOR 2,
// which is also how RT64 reads them. The scratch list lives at physical
// 0x700000, in the top of the 8 MB the runtime reports; the cartridge is a 4 MB
// one that never reads osMemSize, so nothing of the game's is there, and the
// audio task's private copy of its command list is above it at 0x7E0000.

#include "wr64/dlrewrite.h"
#include "wr64/display.h"
#include "wr64/testdrive.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "rt64_extended_gbi.h"

namespace {

constexpr uint32_t kScratch = 0x80700000u;
constexpr uint32_t kScratchSize = 0x40000u;      // 256 KB: 32768 commands
constexpr uint32_t kMaxCommands = kScratchSize / 8;

// Fast3D opcodes this walk has to understand. Everything else is copied.
constexpr uint8_t kOpMtx = 0x01;
constexpr uint8_t kOpMoveMem = 0x03;
constexpr uint8_t kOpDisplayList = 0x06;
constexpr uint8_t kOpMoveWord = 0xBC;
constexpr uint8_t kOpEndDisplayList = 0xB8;
constexpr uint8_t kOpSetScissor = 0xED;     // an RDP command, in 10.2 fixed point

constexpr uint32_t kMtxProjection = 0x01;
constexpr uint32_t kMtxLoad = 0x02;
constexpr uint8_t kMoveWordSegment = 0x06;
constexpr uint8_t kMoveMemViewport = 0x80;

// The framebuffer is 320 wide, and RT64 displaces a viewport by
// origin / 1024 of that width, in quarter pixels: this offset undoes it.
constexpr int16_t kCenterOffset = -static_cast<int16_t>((G_EX_ORIGIN_CENTER * 320 * 4) / G_EX_ORIGIN_RIGHT);

// The states in which the game is racing, and the widened frustum is wanted:
// the attract demo, and the race itself, whichever mode it is in (the game
// keeps one state for a race in every mode and tells them apart elsewhere).
// Everything else -- title, menus, the select screens, results, the ceremony
// -- draws its 3D into a 2D layout.
bool racing(uint32_t state) {
    return state == 0x07 || state == 0x28;
}

struct Walker {
    uint8_t* rdram;
    uint32_t segments[16] = {};
    uint32_t* out;
    uint32_t written = 0;       // commands
    bool overflow = false;

    // The game's most recent viewport command, reissued after an alignment
    // change so that the change takes effect.
    bool have_viewport = false;
    uint32_t viewport_w0 = 0;
    uint32_t viewport_w1 = 0;

    // Whether the list is currently inside a centred perspective section, and
    // whether any 2D (an orthographic projection) has been drawn yet.
    bool centred = false;
    bool seen_ortho = false;

    // The union of the scissors the game set for its drawing this frame. A
    // scissor covering the whole framebuffer is a clear, not drawing, and is
    // left out; what remains is the region the game draws into, which is what
    // the presentation crops to.
    int scissor_left = 0x7FFF, scissor_top = 0x7FFF, scissor_right = -1, scissor_bottom = -1;

    void on_scissor(uint32_t w0, uint32_t w1) {
        const int ulx = static_cast<int>((w0 >> 12) & 0xFFF) >> 2;
        const int uly = static_cast<int>(w0 & 0xFFF) >> 2;
        const int lrx = static_cast<int>((w1 >> 12) & 0xFFF) >> 2;
        const int lry = static_cast<int>(w1 & 0xFFF) >> 2;
        if (ulx == 0 && uly == 0 && lrx >= 319 && lry >= 239) {
            return;
        }
        scissor_left = std::min(scissor_left, ulx);
        scissor_top = std::min(scissor_top, uly);
        scissor_right = std::max(scissor_right, lrx);
        scissor_bottom = std::max(scissor_bottom, lry);
    }

    bool has_scissor() const {
        return scissor_right > scissor_left && scissor_bottom > scissor_top;
    }

    uint32_t physical(uint32_t segmented) const {
        const uint32_t base = segments[(segmented >> 24) & 0xF];
        return (base + (segmented & 0x00FFFFFFu)) & 0x00FFFFF8u;
    }

    const uint32_t* words(uint32_t physical_addr) const {
        return reinterpret_cast<const uint32_t*>(rdram + (physical_addr & 0x00FFFFFFu));
    }

    uint16_t half(uint32_t physical_addr) const {
        return *reinterpret_cast<const uint16_t*>(rdram + ((physical_addr & 0x00FFFFFFu) ^ 2));
    }

    void emit(uint32_t w0, uint32_t w1) {
        if (written >= kMaxCommands) {
            overflow = true;
            return;
        }
        out[written * 2] = w0;
        out[written * 2 + 1] = w1;
        ++written;
    }

    // Room for an extended command of `count` words, as a GfxCommand pointer
    // the header's macros write through; nullptr if it would not fit.
    GfxCommand* reserve(uint32_t count) {
        if (written + count > kMaxCommands) {
            overflow = true;
            return nullptr;
        }
        GfxCommand* cmd = reinterpret_cast<GfxCommand*>(out + written * 2);
        written += count;
        return cmd;
    }

    // RT64's own perspective test, on the fixed-point matrix in RDRAM: [3][3]
    // is 0 for a perspective projection and 1 for an orthographic one.
    bool perspective(uint32_t mtx_physical) const {
        const uint16_t int33 = half(mtx_physical + 30);
        const uint16_t frac33 = half(mtx_physical + 62);
        const uint16_t int11 = half(mtx_physical + 10);
        const uint16_t frac11 = half(mtx_physical + 42);
        return int33 == 0 && frac33 == 0 && (int11 != 0 || frac11 != 0);
    }

    void align_viewport(uint16_t origin, int16_t offset_x) {
        if (GfxCommand* cmd = reserve(2)) {
            gEXSetViewportAlign(cmd, origin, offset_x, 0);
        }
        if (have_viewport) {
            emit(viewport_w0, viewport_w1);
        }
    }
};

}  // namespace

namespace wr64::dlrewrite {

uint32_t rewrite(uint8_t* rdram, uint32_t list_vaddr) {
    static const bool disabled = std::getenv("WR64_NO_REWRITE") != nullptr;
    const uint32_t state = wr64::current_game_state();
    if (disabled) {
        return 0;
    }
    // The walk runs in every state, because the drawn region is read from it;
    // the centring below is for menus only.
    const bool menu = !racing(state);

    Walker w{};
    w.rdram = rdram;
    w.out = reinterpret_cast<uint32_t*>(rdram + (kScratch & 0x00FFFFFFu));

    // RT64 forgets the extended GBI at the end of every list, so every list
    // that uses it has to enable it first.
    if (GfxCommand* cmd = w.reserve(1)) {
        gEXEnable(cmd);
    }

    uint32_t cursor = list_vaddr & 0x00FFFFFFu;
    uint32_t sections = 0;
    for (uint32_t steps = 0; steps < kMaxCommands && !w.overflow; ++steps) {
        const uint32_t* c = w.words(cursor);
        const uint32_t w0 = c[0];
        const uint32_t w1 = c[1];
        const uint8_t op = static_cast<uint8_t>(w0 >> 24);
        cursor += 8;

        if (op == kOpEndDisplayList) {
            w.emit(w0, w1);
            break;
        }

        if (op == kOpDisplayList) {
            const bool branch = ((w0 >> 16) & 0xFF) != 0;
            if (branch) {
                // Continue in place: the copy has no use for the jump.
                cursor = w.physical(w1);
                continue;
            }
            w.emit(w0, w1);
            continue;
        }

        if (op == kOpMoveWord && (w0 & 0xFF) == kMoveWordSegment) {
            w.segments[(w0 >> 10) & 0xF] = w1 & 0x00FFFFFFu;
            w.emit(w0, w1);
            continue;
        }

        if (op == kOpMoveMem && ((w0 >> 16) & 0xFF) == kMoveMemViewport) {
            w.have_viewport = true;
            w.viewport_w0 = w0;
            w.viewport_w1 = w1;
            w.emit(w0, w1);
            continue;
        }

        if (op == kOpSetScissor) {
            w.on_scissor(w0, w1);
            w.emit(w0, w1);
            continue;
        }

        if (op == kOpMtx && menu) {
            const uint32_t params = (w0 >> 16) & 0xFF;
            if ((params & kMtxProjection) && (params & kMtxLoad)) {
                const bool persp = w.perspective(w.physical(w1));
                // Which 3D belongs to the layout is a matter of order. A menu
                // that has a 3D backdrop -- the title, the course overview --
                // draws it first and lays the 2D over it, and that backdrop
                // should stay as wide as the frame. The models the select
                // screens put inside their frames are drawn after the 2D has
                // started. So: 3D before any 2D is a backdrop, 3D after it is
                // part of the layout and is centred.
                if (persp && !w.centred && w.seen_ortho) {
                    w.align_viewport(G_EX_ORIGIN_CENTER, kCenterOffset);
                    w.centred = true;
                    ++sections;
                }
                else if (!persp && w.centred) {
                    w.align_viewport(G_EX_ORIGIN_NONE, 0);
                    w.centred = false;
                }
                if (!persp) {
                    w.seen_ortho = true;
                }
            }
            w.emit(w0, w1);
            continue;
        }

        w.emit(w0, w1);
    }

    if (w.overflow) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            std::fprintf(stderr, "[wr64] a display list did not fit the rewriter's scratch space;"
                                 " it was passed through unchanged\n");
            std::fflush(stderr);
        }
        return 0;
    }

    // The drawn region follows the game's scissor, once it has held for a few
    // frames: a transition frame can draw into less than the whole region, and
    // the crop must not twitch with it.
    if (w.has_scissor()) {
        static int seen_left = -1, seen_top = -1, seen_right = -1, seen_bottom = -1;
        static int seen_for = 0;
        if (w.scissor_left == seen_left && w.scissor_top == seen_top &&
            w.scissor_right == seen_right && w.scissor_bottom == seen_bottom) {
            if (++seen_for == 3) {
                wr64::display::set_content(seen_left, seen_top, seen_right, seen_bottom);
            }
        }
        else {
            seen_left = w.scissor_left;
            seen_top = w.scissor_top;
            seen_right = w.scissor_right;
            seen_bottom = w.scissor_bottom;
            seen_for = 1;
        }
    }

    static uint32_t lists = 0;
    if (++lists == 1 || lists == 600) {
        std::fprintf(stderr, "[wr64] display list rewritten: %u commands, %u centred perspective"
                             " sections (state 0x%02X)\n", w.written, sections, state);
        std::fflush(stderr);
    }
    return kScratch;
}

}  // namespace wr64::dlrewrite
