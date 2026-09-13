#pragma once

// How far away the game keeps drawing things.
//
// **There is no global draw distance in this game.** The far plane is a real
// global and it is already at 16,192, roughly twenty times further out than
// anything the courses contain -- raising it reveals nothing, and only at a
// twentieth of it does the scene begin to clip. What limits the view is the
// game's own culling, decided before anything reaches a display list. A renderer
// cannot put back what was never submitted.
//
// **It is one number, once per view.** This was written first as a list of
// limits that would grow as each was found, on the assumption that each kind of
// object had its own. It does not. `func_8006E674` keeps a buoy only if its
// distance from the camera is less than an integer at `+0xA4` of the course's
// environment struct -- one copy a view, at 0x801CB058 + view * 0x110, which the
// game points 0x801C0C80 at before drawing that view:
//
//     0x8006EC30  mul.s   $f10, $f20, $f20     ; dx^2
//     0x8006EC3C  mul.s   $f6,  $f22, $f22     ; dz^2
//     0x8006EC44  cvt.s.w $f14, $f4            ; (float) the limit
//     0x8006EC4C  jal     0x800C7010           ; sqrtf
//     0x8006EC60  c.lt.s  $f0, $f14            ; distance < limit ?
//     0x8006EC70  bc1fl   L_8006ED20           ; no -> skip this buoy
//
// -- and a census of every display-list call over 5,000 race frames on two
// courses, repeated with that single field doubled, shows **49 of the 57 static
// kinds measured on both runs moved out with it**: buoys, gate markers,
// shoreline props and scenery alike. A second class of larger structures is
// culled at exactly twice the field and so scales with it for free. One integer
// caps nearly all of a course's static geometry, and this scales that integer.
//
// See docs/RENDER-DISTANCE-CENSUS.md for the measurement and
// docs/GAME-INTERNALS.md for the game facts.
//
// **The setting is a distance, not a multiplier**, and that is the other thing
// the census changed. The game's own value is per course -- 5000 on courses 0
// and 1, 6000 on course 2, and 3072 and 2500 seen elsewhere -- so a multiplier
// means something different on every course: two times on a 2500 course only
// reaches what a 5000 course has by default. A distance means the same thing
// everywhere, and it can be checked against what the courses actually contain.
// The furthest any object measured ever sat from the camera was 13,175, so
// **16,192, the far plane, draws everything**: past it the geometry is clipped,
// so no larger number can reveal anything at all. That is the cap, measured,
// replacing an arbitrary four times -- and the only step worth having above the
// game's own, which is why the setting is Original or Extended.
//
// **Original writes nothing.** At the default no memory is touched, so the
// default cannot be the cause of anything. Moving off it and back restores the
// game's own value.
//
// **Not reached by this, and why**
//
// One kind on course 2 -- display list `0x0102CE78` with texture `0x01015220`,
// 53 positions -- is drawn out to about 5,100 and no further, and it did not
// move when `+0xA4` doubled from 6,000 to 12,000: 5,108 against 5,083. So it
// has a limit of its own. Where that limit lives was searched for and is still
// unknown:
//
//   - **Not in the per-course struct.** No field reads between 4,800 and 5,400
//     on any course dumped; the only value near it is `+0xA4` itself.
//   - **Not either of the game's two hardcoded `5000.0f` constants.** One
//     belongs to `func_800A68A4`, which *places* something at x = 5000 and asks
//     the water its height there rather than comparing a distance; the other to
//     `func_800AC184`, in a loop over a table of 0xBC-stride entries.
//
// It is one kind on one course, against the 49 of 57 that `+0xA4` governs.
//
// **The buoys have a second limit: the game's matrix slots**, 32 small buoys and
// 12 racing buoys a view, handed out in table order. Above Original up to 120 are
// inside the distance and the game left buoys as near as 2,032 without a slot.
// patches/buoys.cpp moves their matrices out of the game's buffer and draws every
// one in view; see docs/GAME-INTERNALS.md.
//
// **A trap worth recording, because it nearly cost a measured result.** It is
// tempting to argue such a kind is not distance-culled at all but *selected*,
// the way the direction arrows below are, and there is evidence for it: in 52%
// of frames a nearer instance is skipped while a further one is drawn, which no
// "closer than N" rule can do by itself. But the same is true of the **buoys**,
// whose cap is proven causally, and they skip a nearer buoy in **92%** of
// frames. The game does both -- it caps by distance *and* chooses among what is
// inside the cap. A census verdict built on "skips a nearer one" reclassified
// the buoys, and would have discarded the causal result; the census reports that
// share as a column now and does not rule on it.
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

#include <cstdint>

namespace wr64::drawdistance {

// The furthest the game should keep drawing its course geometry, in world
// units. Zero -- the default -- means the game's own value, and writes nothing.
//
// The number is a floor and a ceiling both: it never lowers a course below what
// the game itself asked for, so the setting cannot hide a buoy a player needs,
// and it never exceeds the far plane, past which nothing can be drawn.
void set_reach(int32_t world_units);

// The largest reach worth asking for: the far plane. Anything drawn beyond this
// is clipped, so a larger number costs submission and shows nothing.
constexpr int32_t kFarPlane = 16192;

// What the setting currently asks for, in world units, or zero for the game's
// own. Read by the water ring, which draws sea out to the same distance and has
// to be off when this is off -- at Original nothing may be added to the frame.
int32_t reach();

// Called once per game frame, from vi_swap_buffer_hook on the game thread
// (patches/framerate.cpp), so each view's copy is written before its next cull.
void apply(uint8_t* rdram);

}  // namespace wr64::drawdistance
