"""Make RT64 say where a rectangle actually lands.

A widescreen problem in the 2D layer looks the same from the outside whatever
causes it: an element that should reach the frame's edges stops short. The port
can say what class it gave the element and what commands it emitted, and a
screenshot can say where the pixels ended up, and neither says what the renderer
did in between -- which of the origins, the aspect flag, the scissor and the
framebuffer width moved the edge, and by how much.

This patches the one place that decides it. `convertViewportRect` turns a
rectangle's own coordinates into a position on the widened framebuffer, and this
prints its inputs and its answer for every rectangle drawn:

    [rt64] rect 0..384 origins 0/1024 aspect 2 ratio 1.000 fb 424 -> x 0.0 w 509.0 (fb viewport 424.0)

Read it as: the game asked for x 0 to 384 of its own screen, the port anchored
the left edge to the frame's left and the right edge to the frame's right, asked
for no aspect squeeze, and RT64 placed it at 0 and made it 509 wide on a
framebuffer 424 wide. An element that covers the frame has x at 0 and x + w at
or past the framebuffer's width; anything else is the shortfall, in the units
that caused it.

Set WR64_RECT_LOG to switch it on; it is silent otherwise, and a race would fill
a terminal in seconds with it on.

Scripted and idempotent because it patches a submodule: a submodule update would
otherwise revert it silently.

Run from the repository root:
    python tools/patch_rt64_rectlog.py
"""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TARGET = REPO / "lib" / "RT64" / "src" / "render" / "rt64_framebuffer_renderer.cpp"

ANCHOR = """                            RenderViewport viewportRect = convertViewportRect(call.callDesc.rect, p.resolutionScale, p.fbWidth, invRatioScale, extOriginPercentage, horizontalMisalignment, call.callDesc.rectLeftOrigin, call.callDesc.rectRightOrigin);"""

REPLACEMENT = """                            RenderViewport viewportRect = convertViewportRect(call.callDesc.rect, p.resolutionScale, p.fbWidth, invRatioScale, extOriginPercentage, horizontalMisalignment, call.callDesc.rectLeftOrigin, call.callDesc.rectRightOrigin);
                            {
                                // Added by the Wave Race 64 port: where a
                                // rectangle actually lands. See
                                // tools/patch_rt64_rectlog.py.
                                static const char *wr64RectLog = std::getenv("WR64_RECT_LOG");
                                if (wr64RectLog != nullptr) {
                                    fprintf(stderr, "[rt64] rect %d..%d origins %u/%u aspect %u ratio %.3f fb %d -> x %.1f w %.1f (fb viewport %.1f) scissor %d..%d\\n",
                                        call.callDesc.rect.left(true), call.callDesc.rect.right(true),
                                        unsigned(call.callDesc.rectLeftOrigin), unsigned(call.callDesc.rectRightOrigin),
                                        unsigned(call.callDesc.rectAspect), invRatioScale, int(p.fbWidth),
                                        viewportRect.x, viewportRect.width, framebuffer.viewport.width,
                                        int(fbPair.scissorRect.ulx), int(fbPair.scissorRect.lrx));
                                }
                            }"""


def main():
    if not TARGET.exists():
        sys.exit(f"missing {TARGET}\nRun: git submodule update --init --recursive")

    text = TARGET.read_text(encoding="utf-8")

    if "WR64_RECT_LOG" in text:
        print(f"  {TARGET.name} already patched")
        return

    if ANCHOR not in text:
        sys.exit(f"anchor not found in {TARGET}; upstream has changed and this "
                 f"patch needs revisiting")

    text = text.replace(ANCHOR, REPLACEMENT, 1)
    if "#include <cstdlib>" not in text:
        text = text.replace("#include ", "#include <cstdlib>\n#include ", 1)

    TARGET.write_text(text, encoding="utf-8")
    print(f"  {TARGET.name} patched")
    print("\nRebuild to pick it up, then run with WR64_RECT_LOG=1.")


if __name__ == "__main__":
    main()
