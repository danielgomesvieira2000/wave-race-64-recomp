#pragma once

// Phase 06: what the game draws, as distinct from what it clears.
//
// Wave Race 64 never draws to its whole 320x240 framebuffer. Every frame --
// title, attract, menus, racing -- is drawn inside the region below, a 303x199
// window with black borders around it, put there for a CRT's overscan to hide.
// Nothing hides them on a modern display: presented as-is they are a black bar
// across the top tenth of the picture and, once the frame is widened, forty
// pixels down each side.
//
// The values are the scissor the game sets, read from RT64 during the title
// screen, the attract sequence and a championship race; all three agree.
// Right and bottom are exclusive, like the scissor's.

namespace wr64::display {

constexpr int kContentLeft = 8;
constexpr int kContentTop = 20;
constexpr int kContentRight = 311;
constexpr int kContentBottom = 219;

// Tells the renderer to present that region, scaled to fit the window, rather
// than the whole framebuffer. Call once, before the first frame.
void crop_to_content();

}  // namespace wr64::display
