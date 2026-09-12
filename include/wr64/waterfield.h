#pragma once

// The game's wave field, dumped so it can be decoded offline.
//
// Wave Race 64's water is not a surface covering the course and it is not
// procedural. It is a **world-fixed, wrapping, triangular lattice** of 64-unit
// cells, and `func_8004F3D4` -- which the decompilation has in C -- gives its
// indexing exactly:
//
//     u   = (s32)(x + z * 0.57735026f) % 24576        // 1/sqrt(3)
//     v   = (s32)(    z * 1.1547005f ) % 24576        // 2/sqrt(3)
//     row = ((v >> 6) + ((u >> 6) & ~0x7F) + 0x600) % 384
//     col =  (u >> 6) & 0x7F
//
//     D_80162420[row * 128 + col] = { s16 height, s16 age }
//
// 384 rows x 128 columns x 4 bytes is 192 KiB, which puts the end of the array
// at exactly 0x80192420 -- the address func_80050204's split-screen test reads,
// and immediately below gWaterLevel at 0x80192458. That agreement is the first
// evidence the model is right; this file exists to produce the rest.
//
// Two things are written, and the second is what makes it a measurement rather
// than a reading of the code:
//
//   - the raw field, so it can be decoded and drawn;
//   - a grid of probes answered by **the game's own height query**,
//     `func_8004D30C(x, z)`, called through the recompiled code.
//
// A decode that reproduces the game's own answers everywhere is a decode that
// can be trusted to build geometry from. One that does not localises the error.
//
// Why this matters for draw distance: the field covers thousands of units and
// is queryable anywhere, while the game *draws* 500 vertices reaching 922. The
// data for a longer view is already there.
//
// Set WR64_WATER_FIELD to a path prefix to arm it. See docs/GAME-INTERNALS.md.

#include <cstdint>

// recomp_context is a typedef of an anonymous struct, so it cannot be forward
// declared -- "struct recomp_context;" is a different type and the compiler
// says so. The header it comes from is included instead, which is why this one
// is only included where recomp.h is already wanted: patches/water.cpp and
// src/waterfield.cpp. It defines MEM_W and friends as macros, so it is not a
// header to spread around.
#include "recomp.h"

namespace wr64::waterfield {

// The field's shape, from func_8004F3D4.
constexpr uint32_t kFieldAddress = 0x80162420;
constexpr uint32_t kFieldRows    = 384;
constexpr uint32_t kFieldCols    = 128;
constexpr uint32_t kCellSize     = 64;
constexpr uint32_t kFieldBytes   = kFieldRows * kFieldCols * 4;

// gWaterLevel: the mean height the field's offsets are added to.
constexpr uint32_t kWaterLevelAddress = 0x80192458;

// Called from the graphics-task hook on the game thread, where a valid context
// is in hand -- calling a recompiled function needs one. Does nothing unless
// WR64_WATER_FIELD is set, and dumps once.
void maybe_dump(uint8_t* rdram, recomp_context* ctx, uint32_t tick, uint32_t course);

}  // namespace wr64::waterfield
