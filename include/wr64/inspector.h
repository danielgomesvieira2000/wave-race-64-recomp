#pragma once

// The HUD inspector: point at an element, see what the port thinks it is, and
// change its class while the game is in front of you.
//
// The widescreen 2D layer is decided by a classifier that runs over every
// display list (src/dlrewrite.cpp), and until now the only way to see what it
// decided was to read a trace afterwards and match it to a screenshot by eye.
// That is a bad instrument for the thing it is measuring: a menu wipe lasts six
// tenths of a second, and an element's identity is not something a picture
// shows. Several confident conclusions drawn that way turned out to be about
// the wrong frame.
//
// So the classifier publishes what it decided, every frame, and RT64's developer
// UI draws it: a list of the frame's elements with their identity, extent,
// projection and class, each with a dropdown that overrides the class on the
// next frame. RT64 supplies the rest of what this needs and already did before
// this existed -- it can pause the game and keep the paused frame interactive,
// and right-clicking a pixel lists the draw calls under it.
//
// Turn it on with WR64_INSPECTOR=1, which also switches on the developer mode
// RT64 needs to draw any UI at all.
//
// Two threads meet here. The classifier runs on the thread that submits display
// lists and fills a frame under construction; the panel runs on the thread that
// owns the renderer's UI and reads the last frame that was completed. They swap
// under a lock, once per frame.

#include <cstdint>

namespace wr64::inspector {

// The classes the rewriter can give an element, in the order the panel offers
// them. Kept as a plain int across this interface so that dlrewrite.cpp's own
// enum stays private to it.
enum Class : int { kAuto = 0, kLeft = 1, kRight = 2, kStretch = 3 };

// Whether the inspector is running at all. Everything below is a no-op when it
// is not, so the calls can sit in the classifier unconditionally.
bool enabled();

// Called once at startup, before the renderer is created.
void init();

// ---- the classifier's side, on the display-list thread ---------------------

void begin_frame(uint32_t game_state);

// One classified element. `identity` is the "tex:0x…" or "dl:0x…" the trace and
// hud.json use; `second_identity` may be empty. The extent is in the game's own
// 320x240 pixels.
void note_element(const char* identity, const char* second_identity,
                  float min_x, float max_x, float min_y, float max_y,
                  bool perspective, int given_class, bool is_rect);

void end_frame();

// Whether the player has overridden this identity in the panel. Consulted by
// the classifier ahead of hud.json, so a change in the panel takes effect on the
// next frame and can be undone without restarting.
bool override_class(const char* identity, int* out_class);

// ---- the panel's side, on the renderer's UI thread --------------------------

// Installs the panel into RT64's inspector. Safe to call when disabled.
void install();

}  // namespace wr64::inspector
