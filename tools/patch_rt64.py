"""Two patches to RT64 that widescreen in Wave Race 64 needs.

Both come from the same fact about this game: it does not draw to its whole
320x240 framebuffer. Every frame -- title, attract, menus, racing -- is drawn
inside the region from (8, 20) to (311, 219), a 303x199 window with black
borders around it that a CRT's overscan was meant to hide. On a modern display
nothing hides them, and RT64 makes two decisions that go wrong because of them.

1. The frame is treated as "not 4:3" and its 2D content is stretched.

   RT64 widens the 3D frustum for widescreen but keeps 2D content -- the HUD --
   at its original shape in the middle of the frame, which is right. It decides
   whether a framebuffer is the game's main 4:3 frame by comparing the scissor's
   shape to 4:3, within 10%. A 303x199 scissor is 1.52, 14% off, so it fails
   the test, and every HUD element is then stretched across the widened frame
   instead: the speed readout ends up at the far right edge and cut off.

   The patch also accepts a scissor that covers most of the framebuffer (three
   quarters of its width and height) as the main frame, whatever its shape.

2. The borders are presented as black bars.

   The final blit maps the whole framebuffer to the window, so the game's own
   borders come out as black bars around the picture: ten percent of the height
   at the top, and the widening multiplier turns the 8-pixel side borders into
   40-pixel ones. The patch lets the port name the region the game draws into,
   and the blit then scales that region to fit the window instead. Vertically
   it fits exactly; horizontally the widened frame has more picture than the
   region, and the window shows as much of it as its shape allows -- the same
   frame a wider CRT would have shown, with nothing black around it.

   The region is set through an extern "C" function so the port needs no RT64
   headers, and it defaults to off, so RT64 behaves as before until a port asks.

Scripted and idempotent because they patch a submodule: a submodule update
would otherwise revert them silently.

Run from the repository root:
    python tools/patch_rt64.py
"""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RT64 = REPO / "lib" / "RT64" / "src"

FB_RENDERER = RT64 / "render" / "rt64_framebuffer_renderer.cpp"
VI_RENDERER = RT64 / "render" / "rt64_vi_renderer.cpp"
VI_HEADER = RT64 / "render" / "rt64_vi_renderer.h"

MARKER = "wr64"

# --- 1. the main-frame test -------------------------------------------------

FB_ANCHOR = """        const bool adjustRatio = (abs((scissorRatio / p.aspectRatioSource) - 1.0f) < SimilarityPercentage);
"""

FB_REPLACEMENT = """        // wr64: a scissor that covers most of the framebuffer is the game's main
        // frame whatever its shape. Wave Race 64 draws inside a 303x199 region
        // of its 320x240 framebuffer, 14% off 4:3, which the similarity test
        // rejects; its 2D content was then stretched across the widened frame
        // instead of kept at its original shape in the middle.
        const bool scissorIsMostOfFramebuffer =
            (fbPair.scissorRect.width(false, true) >= int32_t(p.fbWidth) * 3 / 4) &&
            (fbPair.scissorRect.height(false, true) >= int32_t(p.fbHeight) * 3 / 4);
        const bool adjustRatio = (abs((scissorRatio / p.aspectRatioSource) - 1.0f) < SimilarityPercentage) || scissorIsMostOfFramebuffer;
"""

# --- 2. the content crop ----------------------------------------------------

VI_HEADER_ANCHOR = """        VIRenderer();
        ~VIRenderer();
"""

VI_HEADER_REPLACEMENT = """        // wr64: the region of the framebuffer the game draws into, in framebuffer
        // pixels as left, top, right, bottom with right and bottom exclusive. When
        // set, the blit fits this region to the window rather than the whole
        // framebuffer, so a game's own black borders are not presented. All zero
        // (the default) presents the whole framebuffer as before.
        static hlslpp::float4 contentCrop;

        VIRenderer();
        ~VIRenderer();
"""

VI_FUNC_ANCHOR = """    void VIRenderer::render(const RenderParams &p) {
"""

VI_FUNC_REPLACEMENT = """    // wr64: see contentCrop.
    inline hlslpp::float2 fromHDtoWindowCropped(hlslpp::float2 coordinate, hlslpp::float2 sdSize, hlslpp::float2 hdSize, hlslpp::float2 windowSize, hlslpp::float4 cropSD) {
        // The content region in the virtual HD TV's space. The frame may have been
        // widened (Aspect Ratio: Expand), in which case the horizontal scale is not
        // the vertical one; the region is placed the way the renderer places 2D
        // content, at the vertical scale and centered, so what fills the window is
        // the game's own frame plus however much widened picture fits beside it.
        const float scale = float(hdSize.y) / float(sdSize.y);
        const float sdCenterX = float(sdSize.x) / 2.0f;
        const float hdCenterX = float(hdSize.x) / 2.0f;
        const float x0 = hdCenterX + (float(cropSD.x) - sdCenterX) * scale;
        const float x1 = hdCenterX + (float(cropSD.z) - sdCenterX) * scale;
        const float y0 = float(cropSD.y) * scale;
        const float y1 = float(cropSD.w) * scale;
        const float relativeScale = std::min(float(windowSize.x) / (x1 - x0), float(windowSize.y) / (y1 - y0));
        const float contentCenterX = (x0 + x1) / 2.0f;
        const float contentCenterY = (y0 + y1) / 2.0f;
        return {
            float(windowSize.x) / 2.0f + (float(coordinate.x) - contentCenterX) * relativeScale,
            float(windowSize.y) / 2.0f + (float(coordinate.y) - contentCenterY) * relativeScale
        };
    }

    hlslpp::float4 VIRenderer::contentCrop = hlslpp::float4(0.0f, 0.0f, 0.0f, 0.0f);

    extern "C" void RT64_SetVIContentCrop(float left, float top, float right, float bottom) {
        VIRenderer::contentCrop = hlslpp::float4(left, top, right, bottom);
    }

    void VIRenderer::render(const RenderParams &p) {
"""

VI_MAP_ANCHOR = """        // Scale all the rectangles to the space of the Window.
        hlslpp::float2 topLeftViewport = fromSDtoHD({ float(viViewRect.x), float(viViewRect.y) }, sdSize, hdSize);
        hlslpp::float2 bottomRightViewport = fromSDtoHD({ float(viViewRect.x + viViewRect.z), float(viViewRect.y + viViewRect.w) }, sdSize, hdSize);
        topLeftViewport = fromHDtoWindow(topLeftViewport, hdSize, windowSize);
        bottomRightViewport = fromHDtoWindow(bottomRightViewport, hdSize, windowSize);

        hlslpp::float2 topLeftScissor = fromSDtoHD({ float(viCropRect.x), float(viCropRect.y) }, sdSize, hdSize);
        hlslpp::float2 bottomRightScissor = fromSDtoHD({ float(viCropRect.x + viCropRect.z), float(viCropRect.y + viCropRect.w) }, sdSize, hdSize);
        topLeftScissor = fromHDtoWindow(topLeftScissor, hdSize, windowSize);
        bottomRightScissor = fromHDtoWindow(bottomRightScissor, hdSize, windowSize);

        viewport = RenderViewport(topLeftViewport.x, topLeftViewport.y, bottomRightViewport.x - topLeftViewport.x, bottomRightViewport.y - topLeftViewport.y);
        scissor = RenderRect(lround(topLeftScissor.x), lround(topLeftScissor.y), lround(bottomRightScissor.x), lround(bottomRightScissor.y));
"""

VI_MAP_REPLACEMENT = """        // wr64: fit the game's content region to the window when a port has set one.
        const hlslpp::float4 crop = contentCrop;
        const bool useCrop = (float(crop.z) > float(crop.x)) && (float(crop.w) > float(crop.y));
        auto toWindow = [&](hlslpp::float2 coordinate) {
            return useCrop ? fromHDtoWindowCropped(coordinate, sdSize, hdSize, windowSize, crop) : fromHDtoWindow(coordinate, hdSize, windowSize);
        };

        // Scale all the rectangles to the space of the Window.
        hlslpp::float2 topLeftViewport = fromSDtoHD({ float(viViewRect.x), float(viViewRect.y) }, sdSize, hdSize);
        hlslpp::float2 bottomRightViewport = fromSDtoHD({ float(viViewRect.x + viViewRect.z), float(viViewRect.y + viViewRect.w) }, sdSize, hdSize);
        topLeftViewport = toWindow(topLeftViewport);
        bottomRightViewport = toWindow(bottomRightViewport);

        hlslpp::float2 topLeftScissor = fromSDtoHD({ float(viCropRect.x), float(viCropRect.y) }, sdSize, hdSize);
        hlslpp::float2 bottomRightScissor = fromSDtoHD({ float(viCropRect.x + viCropRect.z), float(viCropRect.y + viCropRect.w) }, sdSize, hdSize);
        topLeftScissor = toWindow(topLeftScissor);
        bottomRightScissor = toWindow(bottomRightScissor);

        viewport = RenderViewport(topLeftViewport.x, topLeftViewport.y, bottomRightViewport.x - topLeftViewport.x, bottomRightViewport.y - topLeftViewport.y);
        scissor = RenderRect(lround(topLeftScissor.x), lround(topLeftScissor.y), lround(bottomRightScissor.x), lround(bottomRightScissor.y));

        // wr64: the crop pushes the VI's rectangle past the window's edges, and a
        // scissor must stay inside the render target.
        if (useCrop) {
            scissor.left = std::max(scissor.left, 0);
            scissor.top = std::max(scissor.top, 0);
            scissor.right = std::min(scissor.right, int32_t(windowSize.x));
            scissor.bottom = std::min(scissor.bottom, int32_t(windowSize.y));
        }
"""


def patch(target, anchor, replacement, name):
    text = target.read_text()
    if replacement in text:
        print(f"  {target.name}: {name} already patched")
        return
    if anchor not in text:
        sys.exit(f"anchor for {name} not found in {target}; upstream has changed "
                 f"and this patch needs revisiting")
    target.write_text(text.replace(anchor, replacement, 1))
    print(f"  {target.name}: {name} patched")


def main():
    for target in (FB_RENDERER, VI_RENDERER, VI_HEADER):
        if not target.exists():
            sys.exit(f"missing {target}. Run: git submodule update --init --recursive")

    patch(FB_RENDERER, FB_ANCHOR, FB_REPLACEMENT, "main-frame test")
    patch(VI_HEADER, VI_HEADER_ANCHOR, VI_HEADER_REPLACEMENT, "content crop (header)")
    patch(VI_RENDERER, VI_FUNC_ANCHOR, VI_FUNC_REPLACEMENT, "content crop (setter)")
    patch(VI_RENDERER, VI_MAP_ANCHOR, VI_MAP_REPLACEMENT, "content crop (blit)")
    print("Rebuild to pick it up.")


if __name__ == "__main__":
    main()
