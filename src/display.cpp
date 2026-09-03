// Phase 06: present only the region of the framebuffer the game draws into.
//
// See include/wr64/display.h for what the region is and why it exists. The
// cropping itself is a small addition to RT64's final blit, applied by
// tools/patch_rt64.py; it is exposed as a C function so this file needs none
// of RT64's headers.

#include "wr64/display.h"

#include <cstdio>

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

}  // namespace wr64::display
