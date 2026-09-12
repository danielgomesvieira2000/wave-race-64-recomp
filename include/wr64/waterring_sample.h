#pragma once

// The sampling half of the water ring: see wr64/waterring.h for what the ring
// is and why it exists.
//
// This is separate because calling the game's own height query needs a
// recomp_context, which means including recomp.h -- and recomp.h defines MEM_W
// and friends as macros, which must not reach the display-list rewriter. Only
// the graphics-task hook in patches/water.cpp includes this.

#include "wr64/waterring.h"

#include "recomp.h"

namespace wr64::waterring {

// Called from the graphics-task hook on the game thread, beside
// water::publish_frame, with the display list this task will run. Samples the
// ring's vertices against the game's own wave field and publishes them for the
// rewriter to claim. Does nothing unless the modern water renderer is on.
void publish(uint8_t* rdram, recomp_context* ctx, uint32_t display_list);

}  // namespace wr64::waterring
