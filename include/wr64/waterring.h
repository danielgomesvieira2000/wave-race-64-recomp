#pragma once

// Water past the edge of the game's own patch.
//
// The game animates a 500-vertex patch that reaches **922 units** around the
// camera, and that is all the water there is: a whole-frame trace found nothing
// else within two units of sea level except the craft. What fills the screen
// from the patch's edge down to the bottom is the lowest of the sky's three
// bands -- a seven-vertex plane at y = 0, painted to look like distant sea. See
// docs/GAME-INTERNALS.md, *The water*.
//
// So there is no far surface to shade, and making the sea reach further means
// putting geometry where the game draws none. That is what this builds: a ring
// of quads from the patch's edge out to the distance the rest of the course is
// drawn at, standing on the **same wave field the game's own patch stands on**,
// so the swell outside agrees with the swell inside instead of being invented.
//
// **Why the heights are sampled here and not in the renderer.** The field lives
// in RDRAM and the game thread writes it. The display-list rewriter runs on the
// graphics thread, so reading the field there would be a torn read of a moving
// structure. This follows the pattern water.cpp already uses: sample on the game
// thread at task submission, publish keyed by display list, and let the rewriter
// claim it. What is sampled is only the ring's own vertices -- a few hundred
// points -- not the field's 192 KiB.
//
// The height comes from **the game's own query**, `func_8004D30C(x, z)`, rather
// than from a second implementation of the lattice arithmetic in C++. A decode
// exists and is verified (tools/decode_water_field.py), but a copy is a thing
// that can drift; the game's own answer cannot.

#include <cstdint>

// This half is included by the display-list rewriter, which must not see
// recomp.h: that header defines MEM_W and friends as macros. The sampling half,
// which needs a recomp_context to call the game's own height query, is declared
// in wr64/waterring_sample.h instead. recomp_context is a typedef of an
// anonymous struct and cannot be forward declared, so the split is the only way
// to keep the macros out of here -- wr64/waterfield.h has the same constraint.

namespace wr64::waterring {

// The ring's shape. Twenty-four sectors is 15 degrees each, which at the inner
// radius is about 240 units between neighbours -- coarser than the game's own
// 32-unit grid, and correct for it: this geometry is never closer than 922
// units, where a wave is a few pixels across.
constexpr int kSectors = 24;

// Radii, including the inner edge. Six bands of quads between seven circles,
// spaced so they grow with distance: the far bands cover far more ground for
// the same vertex count, which is where the cost has to go.
constexpr int kCircles = 7;

constexpr int kVertexCount = kSectors * kCircles;

// One vertex, in world space, exactly as a Vtx holds it. The game's own water
// is drawn under an identity model matrix with world-space vertices, so this
// can be too -- and 6,000 units fits an int16 with room to spare.
struct Vertex {
    int16_t x, y, z;
};

struct Ring {
    Vertex vertices[kVertexCount];
    // The inner radius the ring starts at, so the rewriter can say in a log what
    // it drew without recomputing it.
    float inner;
    float outer;
};

// Claimed by the display-list rewriter on the graphics thread, by the same list
// identity water::begin_frame uses. Null when there is nothing to draw.
const Ring* claim(uint32_t display_list);

// WR64_NO_WATER_RING=1 switches it off, as CONTRIBUTING.md asks of anything that
// could make a machine worse.
bool enabled();

}  // namespace wr64::waterring
