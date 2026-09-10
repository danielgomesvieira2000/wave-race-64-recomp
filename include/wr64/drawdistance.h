#pragma once

// How far away the game keeps drawing things.
//
// **There is no global draw distance in this game.** The far plane is a real
// global and it is already at 16,192, roughly twenty times further out than
// anything the courses contain -- raising it reveals nothing, and only at a
// twentieth of it does the scene begin to clip. What limits the view is the
// game's own culling, decided per kind of object, in that object's own drawing
// code, before anything reaches a display list. A renderer cannot put back what
// was never submitted.
//
// So this is one setting over a list of limits, and the list grows as each one is
// found. Each entry is a number the game keeps in memory and compares a distance
// against; the setting scales every one of them, from whatever the game itself
// last wrote, and restores them when it goes back to Original.
//
// **In the list**
//
// The buoys. `func_8006E674` builds the list to draw and keeps each buoy only if
// its distance from the camera is less than an integer at `+0xA4` of a per-course
// struct, whose address the game leaves at `0x801C0C80`:
//
//     0x8006EC30  mul.s   $f10, $f20, $f20     ; dx^2
//     0x8006EC3C  mul.s   $f6,  $f22, $f22     ; dz^2
//     0x8006EC44  cvt.s.w $f14, $f4            ; (float) the limit
//     0x8006EC4C  jal     0x800C7010           ; sqrtf
//     0x8006EC60  c.lt.s  $f0, $f14            ; distance < limit ?
//     0x8006EC70  bc1fl   L_8006ED20           ; no -> skip this buoy
//
// It reads 5000. At four times, the count drawn went from 12-23 to 40-54 and the
// furthest from 4,096-4,570 to 7,062-8,617 -- the far side of the course, which
// is why four is the cap.
//
// **Not in the list, and why**
//
// The arrows and signs at the gates are culled at roughly 1,600 by code that has
// not been found; turning this up leaves them vanishing in the same place.
//
// The animated water is not culled at all. It is *generated*: a fixed 500-vertex
// patch reaching 922 units, rebuilt around the camera every frame, whose spacing
// is computed rather than stored. There is no number to scale.
//
// Both are written up in docs/GAME-INTERNALS.md with what has been ruled out.

#include <cstdint>

namespace wr64::drawdistance {

// The multiplier on the game's own limits. One is the game's own behaviour and
// costs nothing: no memory is written at all.
void set_multiplier(double value);

// Called once per display list, from the thread that submits them.
void apply(uint8_t* rdram);

}  // namespace wr64::drawdistance
