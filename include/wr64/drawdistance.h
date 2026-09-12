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
// **In the list: one number, and it is not only the buoys'**
//
// `func_8006E674` builds the list of buoys to draw and keeps each one only if its
// distance from the camera is less than an integer at `+0xA4` of a per-course
// struct, whose address the game leaves at `0x801C0C80`:
//
//     0x8006EC30  mul.s   $f10, $f20, $f20     ; dx^2
//     0x8006EC3C  mul.s   $f6,  $f22, $f22     ; dz^2
//     0x8006EC44  cvt.s.w $f14, $f4            ; (float) the limit
//     0x8006EC4C  jal     0x800C7010           ; sqrtf
//     0x8006EC60  c.lt.s  $f0, $f14            ; distance < limit ?
//     0x8006EC70  bc1fl   L_8006ED20           ; no -> skip this buoy
//
// That was found by reading the buoy code, and calling the entry "the buoys" was
// too small a claim. A census of every display-list call over 5,000 race frames
// on two courses, and the same census with this one field doubled, says what it
// actually governs: **49 of the 57 static kinds measured on both runs moved out
// with it**, buoys, gate markers, shoreline props and course scenery alike. One
// number caps nearly all of the course's static geometry.
//
// It is per course -- 5000 on courses 0 and 1, 6000 on course 2 -- and the census
// recovers it from the drawing alone, without being told: the buoys' 99.5th
// percentile reach is 4,997 on course 1 and 5,969 on course 2.
//
// **A second class sits at exactly twice it.** On course 1, with the field at
// 5000, a family of larger structures reaches 9,916 and is never drawn beyond,
// though its sites get 13,175 away. Doubling the field to 10000 lifts that class
// past what the course contains at all, which is what a limit of 20,000 would do.
// So the multiplier already scales both; they are one setting, not two.
//
// The measured tail is about 9% over the limit -- 23,526 buoy draws top out at
// 5,464 against 5,000 -- because the cull runs when the list is *built* and the
// object is still in the list a frame or two later. That is not slack to scale
// into; it is the same limit, measured a frame late.
//
// At four times, 5000 to 20000, the count of buoys drawn went from 12-23 to 40-54
// and the furthest from 4,096-4,570 to 7,062-8,617 -- the far side of the course.
// That is why four is the cap: there is nothing further out to reveal.
//
// **Not in the list, and why**
//
// The course's yellow direction arrows are not culled by distance at all, so
// there is nothing here to scale. Measured over a lap: one or two drawn per
// frame out of thirteen, and the drawn ones are not the nearest -- one at 531
// units gets skipped while one at 4,923 is drawn. They are navigational markers
// selected by where the player is on the course, not scenery being dropped.
//
// The animated water is not culled at all. It is *generated*: a fixed 500-vertex
// patch reaching 922 units, rebuilt around the camera every frame, whose spacing
// is computed rather than stored. There is no number to scale.
//
// **Still missing a number.** One static kind on course 2 -- display list
// `0x0102CE78` with texture `0x01015220`, 746 sites -- is culled at about 5,100
// and did **not** move when `+0xA4` doubled (5,108 to 5,083). It has its own
// limit somewhere else, and until that is found this setting does not reach it.
//
// All of this is written up in docs/GAME-INTERNALS.md, and the measurement that
// produced it in docs/RENDER-DISTANCE-CENSUS.md.

#include <cstdint>

namespace wr64::drawdistance {

// The multiplier on the game's own limits. One is the game's own behaviour and
// costs nothing: no memory is written at all.
void set_multiplier(double value);

// Called once per display list, from the thread that submits them.
void apply(uint8_t* rdram);

}  // namespace wr64::drawdistance
