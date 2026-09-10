#pragma once

// How far away the game keeps drawing its course objects.
//
// The far plane is not what limits the view here -- it sits at 16,192 while the
// courses need a few hundred (see docs/GAME-INTERNALS.md, *The world's
// frustum*). What limits it is the game's own culling, decided before anything
// reaches a display list, so a renderer cannot put back what was never
// submitted.
//
// `func_8006E674` builds the list of buoys to draw. For each one it takes the
// distance from the camera and keeps it only if that is less than an integer
// read from a per-course struct, whose address the game leaves at 0x801C0C80:
//
//     0x8006EC30  mul.s   $f10, $f20, $f20     ; dx^2
//     0x8006EC3C  mul.s   $f6,  $f22, $f22     ; dz^2
//     0x8006EC44  cvt.s.w $f14, $f4            ; (float) struct[+0xA4]
//     0x8006EC48  add.s   $f12, $f10, $f6
//     0x8006EC4C  jal     0x800C7010           ; sqrtf
//     0x8006EC60  c.lt.s  $f0, $f14            ; distance < limit ?
//     0x8006EC70  bc1fl   L_8006ED20           ; no -> skip this buoy
//
// The limit reads 5000 on the courses measured. Raising it is the whole fix:
// the game then submits the buoys it was skipping, and everything downstream --
// matrices, display list, the renderer -- follows on its own.
//
// The struct belongs to the course and the game rewrites it, so the value is
// reapplied every frame from the original the game last wrote, rather than
// scaled once and left to compound.

#include <cstdint>

namespace wr64::drawdistance {

// The multiplier on the game's own limit. One is the game's own behaviour and
// costs nothing: no memory is written at all.
void set_multiplier(double value);

// Called once per display list, from the thread that submits them.
void apply(uint8_t* rdram);

}  // namespace wr64::drawdistance
