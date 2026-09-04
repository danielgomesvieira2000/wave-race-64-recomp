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

struct SDL_Window;

namespace wr64::display {

constexpr int kContentLeft = 8;
constexpr int kContentTop = 20;
constexpr int kContentRight = 311;
constexpr int kContentBottom = 219;

// The region is not fixed after all: with two players the game draws into
// (8, 12)-(311, 229), taller than the single-player region, and a crop pinned
// to the latter cut the top and bottom off the split screen. The display-list
// rewriter reads the scissor the game sets each frame and reports the drawn
// region here; the crop follows it, and a change is logged once.
void set_content(int left, int top, int right, int bottom);

// The window, for its shape. The display-list rewriter needs to know how much
// of RT64's widened frame the presentation shows, and that follows from the
// window's aspect ratio and the region above.
void set_window(::SDL_Window* window);

// How far, in framebuffer pixels, the visible picture's left edge lies inside
// RT64's widened frame, less the game's own left border: the offset that puts
// an element anchored to RT64's left edge at the same distance from the
// picture's edge that the game gave it from its content edge. Zero when the
// frame is not widened. The right edge mirrors it.
float anchor_inset();

// Tells the renderer to present that region, scaled to fit the window, rather
// than the whole framebuffer. Call once, before the first frame.
void crop_to_content();

}  // namespace wr64::display
