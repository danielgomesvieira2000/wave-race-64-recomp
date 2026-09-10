#pragma once

#include <cstdint>

// The display-list rewriter: 0.2's tool for the 2D layer and for the frames
// RT64 draws in between the game's.
//
// The cartridge predates RT64's extended GBI, so it never says which of its
// draws is a HUD element to be kept at an edge, which projection belongs to a
// menu, or which matrix is which object from one frame to the next. RT64
// guesses well enough for most of a race and badly on the menus. Rather than
// patch the game's undecompiled drawing code, the port copies each graphics
// task's display list into scratch RDRAM and inserts the extended commands
// where they are needed, before RT64 sees the list. The game's code and data
// are untouched; RT64 honours the commands from any list that begins with its
// enable command, and the Fast3D microcode leaves the opcode free.
//
// See docs/PLAN.md, phase 07, for what is inserted and why.
namespace wr64::dlrewrite {

// Rewrites the top-level list at `list_vaddr` into scratch RDRAM and returns
// the scratch list's address, or 0 if the list was left as it was -- because
// it did not fit, or because nothing in it needed changing. The scratch copy
// is valid until the next call.
uint32_t rewrite(uint8_t* rdram, uint32_t list_vaddr);

// The vertical field of view the world is drawn with, in degrees. The game's
// own is 45; wider shows more without changing the shape of anything, because
// the horizontal half is derived from it and the aspect ratio.
//
// Called from the settings menu, read on the thread that submits display lists.
void set_field_of_view(double degrees);

}  // namespace wr64::dlrewrite
