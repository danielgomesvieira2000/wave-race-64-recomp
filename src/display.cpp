// Phase 06: present only the region of the framebuffer the game draws into.
//
// See include/wr64/display.h for what the region is and why it exists. The
// cropping itself is a small addition to RT64's final blit, applied by
// tools/patch_rt64.py; it is exposed as a C function so this file needs none
// of RT64's headers.

#include "wr64/display.h"

#include <algorithm>
#include <cstdio>

#include <SDL.h>

extern "C" void RT64_SetVIContentCrop(float left, float top, float right, float bottom);

namespace wr64::display {

void crop_to_content() {
    RT64_SetVIContentCrop(float(kContentLeft), float(kContentTop),
                          float(kContentRight), float(kContentBottom));
    std::fprintf(stderr, "[wr64] presenting the game's %dx%d drawn region, not its %s\n",
                 kContentRight - kContentLeft, kContentBottom - kContentTop,
                 "320x240 framebuffer with the borders a CRT would have hidden");
    std::fflush(stderr);
}

namespace {
int g_left = kContentLeft, g_top = kContentTop, g_right = kContentRight, g_bottom = kContentBottom;
SDL_Window* g_window = nullptr;
}  // namespace

void set_content(int left, int top, int right, int bottom) {
    if (left == g_left && top == g_top && right == g_right && bottom == g_bottom) {
        return;
    }
    g_left = left;
    g_top = top;
    g_right = right;
    g_bottom = bottom;
    RT64_SetVIContentCrop(float(left), float(top), float(right), float(bottom));
    std::fprintf(stderr, "[wr64] the game now draws into (%d, %d)-(%d, %d); presenting that region\n",
                 left, top, right, bottom);
    std::fflush(stderr);
}

void set_window(SDL_Window* window) {
    g_window = window;
}

// The presentation (see tools/patch_rt64.py) fits the drawn region to the
// window: the region is placed at the unwidened scale in the middle of RT64's
// widened frame, and scaled so that it fills the window's height, or its
// width if the window is narrower than the region. What the window then shows
// of the widened frame is, in framebuffer pixels,
//
//     max(region width, region height * window aspect)
//
// out of the frame's 320 * ratio, where ratio is how much RT64 widened it:
// the window's aspect over the framebuffer's 4:3. Half the difference is how
// far the visible edge lies inside the frame's edge.
float anchor_inset() {
    int w = 0, h = 0;
    if (g_window != nullptr) {
        SDL_GetWindowSize(g_window, &w, &h);
    }
    if (w <= 0 || h <= 0) {
        return 0.0f;
    }
    const float window_aspect = float(w) / float(h);
    const float ratio = window_aspect / (320.0f / 240.0f);
    if (ratio <= 1.0f) {
        return 0.0f;
    }
    const float region_width = float(g_right - g_left);
    const float region_height = float(g_bottom - g_top);
    const float visible = std::max(region_width, region_height * window_aspect);
    const float overhang = (320.0f * ratio - visible) / 2.0f;
    return std::max(overhang - float(g_left), 0.0f);
}

}  // namespace wr64::display
