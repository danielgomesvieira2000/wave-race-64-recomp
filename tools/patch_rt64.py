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

3. A 3D pass that covers the drawn region is not widened, in some frames.

   RT64 widens a 3D pass only when it reaches both edges of the frame it is
   drawing into. This game scissors its world to (8, 20)-(311, 219), and in
   most frames nothing else touches the framebuffer, so the frame's scissor is
   that same region and the test passes. In the championship's warm-up round
   something else in the frame touches the whole 320x240 framebuffer: the
   frame's scissor becomes the whole thing, the world falls eight pixels short
   at each end, and the warm-up alone was rendered at 4:3. The patch allows a
   sixteenth of the frame's width in tolerance -- about twenty pixels, more
   than the border and far less than the inset boxes the select screens draw
   their models into, which must keep failing the test. RT64 asks the question
   in two places -- once to render the pass across the widened frame, once to
   widen the frustum that fills it -- and both are patched: answering only the
   first stretches the game's 4:3 frustum across a wide viewport, which looks
   like a stretched image rather than a wider view.

4. There is no way to tell how well interpolation is doing.

   RT64 interpolates an object by pairing its transform with the previous
   frame's, and an object that finds no pair is drawn at the newer frame and
   holds there. Nothing reports how often that happens, so every change to
   interpolation had to be argued rather than measured. The patch counts, per
   frame, how many world transforms there were, how many found no pair, and how
   many of those had a matrix that appears nowhere in the previous frame -- the
   last being the number that costs something, since an unpaired object that is
   not moving looks no different for it. A port reads the running totals through
   an extern "C" accessor and reports the rates; see patches/framerate.cpp.

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
PROJ_PROCESSOR = RT64 / "render" / "rt64_projection_processor.cpp"

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


# --- 3. widening a 3D pass that covers the drawn region ---------------------

WIDEN_ANCHOR = """                bool coversWholeWidth = !intersectionRect.isEmpty() && (intersectionRect.ulx <= fbPair.scissorRect.ulx) && (intersectionRect.lrx >= fbPair.scissorRect.lrx);
"""

WIDEN_REPLACEMENT = """                // wr64: a 3D pass that reaches nearly both edges of the frame is
                // covering it. The exact test fails on this game's eight-pixel
                // border. It scissors its world to the region it draws into,
                // (8, 20)-(311, 219), and in most frames nothing else touches
                // the framebuffer, so the frame's scissor is that same region
                // and the test passes. In the championship's warm-up round
                // something else in the frame does touch the whole 320x240
                // framebuffer; the frame's scissor becomes the whole thing, the
                // world falls eight pixels short at each end, and the warm-up
                // was rendered at 4:3 while every other race was widened.
                //
                // A sixteenth of the frame's width is about twenty pixels here:
                // comfortably more than the border, and far less than the inset
                // boxes the select screens draw their models into, which reach
                // barely half the width and must keep failing this test.
                const int32_t coverTolerance = (fbPair.scissorRect.lrx - fbPair.scissorRect.ulx) / 16;
                bool coversWholeWidth = !intersectionRect.isEmpty() && (intersectionRect.ulx <= fbPair.scissorRect.ulx + coverTolerance) && (intersectionRect.lrx >= fbPair.scissorRect.lrx - coverTolerance);
"""


PROJ_ANCHOR = """                    bool coversWholeWidth = (intersectionRect.ulx <= fbPair.scissorRect.ulx) && (intersectionRect.lrx >= fbPair.scissorRect.lrx);
"""

PROJ_REPLACEMENT = """                    // wr64: the same tolerance as the widening test above, and it
                    // has to be here as well. RT64 asks this question twice: once
                    // to decide whether to render the pass across the widened
                    // frame, and once to decide whether to widen the frustum that
                    // fills it. Answering only the first leaves the game's 4:3
                    // frustum stretched across a wide viewport, which is a wider
                    // picture of the same view rather than more of the view -- it
                    // reads as a horizontally stretched image, and that is exactly
                    // how the warm-up round looked when only the other one was
                    // patched.
                    const int32_t coverTolerance = (fbPair.scissorRect.lrx - fbPair.scissorRect.ulx) / 16;
                    bool coversWholeWidth = (intersectionRect.ulx <= fbPair.scissorRect.ulx + coverTolerance) && (intersectionRect.lrx >= fbPair.scissorRect.lrx - coverTolerance);
"""


# --- 4. the interpolation measurement ---------------------------------------

GAME_FRAME = RT64 / "hle" / "rt64_game_frame.cpp"

PAIRING_INCLUDE_ANCHOR = """#include "xxHash/xxh3.h"
"""

PAIRING_INCLUDE_REPLACEMENT = """#include "xxHash/xxh3.h"

// wr64: for the transform-pairing counters below.
#include <unordered_set>
"""

PAIRING_ANCHOR = """        matchScenes(perspectiveScenes, prevFrame.perspectiveScenes);
        matchScenes(orthographicScenes, prevFrame.orthographicScenes);
"""

PAIRING_REPLACEMENT = """        matchScenes(perspectiveScenes, prevFrame.perspectiveScenes);
        matchScenes(orthographicScenes, prevFrame.orthographicScenes);

        // wr64: count what failed to pair, so the port can report it.
        //
        // RT64 interpolates an object by pairing this frame's transform with
        // the previous frame's; one that finds no pair is drawn at the newer
        // frame and holds there (see TransformProcessor::process). The plain
        // count of those is a poor measure, because most of them are the
        // course's static scenery -- the same matrix every frame, often
        // submitted twice, which is exactly what ties a matcher that goes by
        // position -- and an unpaired object that is not moving looks no
        // different for it.
        //
        // So they are counted twice: every transform that found no pair, and
        // the subset whose matrix appears nowhere in the previous frame at
        // all. The second number is the one that costs something, because an
        // object that is both moving and unpaired is an object stepping at the
        // game's rate while everything around it glides.
        {
            thread_local std::unordered_set<uint64_t> wr64PrevMatrices;
            wr64PrevMatrices.clear();
            for (uint32_t w : workloads) {
                const GameFrameMap::WorkloadMap &map = frameMap.workloads[w];
                if (!map.mapped) {
                    continue;
                }

                const Workload &prevWorkload = workloadQueue.workloads[map.prevWorkloadIndex];
                for (const interop::float4x4 &m : prevWorkload.drawData.worldTransforms) {
                    wr64PrevMatrices.insert(XXH3_64bits(&m, sizeof(m)));
                }
            }

            uint32_t total = 0, unpaired = 0, unpairedMoved = 0;
            for (uint32_t w : workloads) {
                const GameFrameMap::WorkloadMap &map = frameMap.workloads[w];
                const Workload &curWorkload = workloadQueue.workloads[w];
                for (size_t t = 0; t < map.transforms.size(); t++) {
                    total++;
                    if (map.transforms[t].mapped) {
                        continue;
                    }

                    unpaired++;
                    const interop::float4x4 &m = curWorkload.drawData.worldTransforms[t];
                    if (wr64PrevMatrices.find(XXH3_64bits(&m, sizeof(m))) == wr64PrevMatrices.end()) {
                        unpairedMoved++;
                    }
                }
            }

            wr64PairingFrames++;
            wr64PairingTotal += total;
            wr64PairingUnpaired += unpaired;
            wr64PairingUnpairedMoved += unpairedMoved;
        }
"""

PAIRING_COUNTERS_ANCHOR = """namespace RT64 {
    // GameFrame
"""

PAIRING_COUNTERS_REPLACEMENT = """// wr64: running totals of the transform pairing, read by the port through the
// accessor below. Written on the workload thread and read on the game thread
// without synchronisation, which is sound enough for a counter that is only
// ever reported: the reader wants a rate over seconds, not an exact instant.
static uint64_t wr64PairingFrames = 0;
static uint64_t wr64PairingTotal = 0;
static uint64_t wr64PairingUnpaired = 0;
static uint64_t wr64PairingUnpairedMoved = 0;

extern "C" void RT64_GetTransformPairing(unsigned long long *frames, unsigned long long *total,
                                         unsigned long long *unpaired, unsigned long long *unpairedMoved) {
    *frames = wr64PairingFrames;
    *total = wr64PairingTotal;
    *unpaired = wr64PairingUnpaired;
    *unpairedMoved = wr64PairingUnpairedMoved;
}

namespace RT64 {
    // GameFrame
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
    for target in (FB_RENDERER, VI_RENDERER, VI_HEADER, PROJ_PROCESSOR, GAME_FRAME):
        if not target.exists():
            sys.exit(f"missing {target}. Run: git submodule update --init --recursive")

    patch(FB_RENDERER, FB_ANCHOR, FB_REPLACEMENT, "main-frame test")
    patch(FB_RENDERER, WIDEN_ANCHOR, WIDEN_REPLACEMENT, "widening test")
    patch(PROJ_PROCESSOR, PROJ_ANCHOR, PROJ_REPLACEMENT, "widening test (frustum)")
    patch(VI_HEADER, VI_HEADER_ANCHOR, VI_HEADER_REPLACEMENT, "content crop (header)")
    patch(VI_RENDERER, VI_FUNC_ANCHOR, VI_FUNC_REPLACEMENT, "content crop (setter)")
    patch(VI_RENDERER, VI_MAP_ANCHOR, VI_MAP_REPLACEMENT, "content crop (blit)")
    patch(GAME_FRAME, PAIRING_INCLUDE_ANCHOR, PAIRING_INCLUDE_REPLACEMENT, "pairing counters (include)")
    patch(GAME_FRAME, PAIRING_COUNTERS_ANCHOR, PAIRING_COUNTERS_REPLACEMENT, "pairing counters (accessor)")
    patch(GAME_FRAME, PAIRING_ANCHOR, PAIRING_REPLACEMENT, "pairing counters (count)")
    print("Rebuild to pick it up.")


if __name__ == "__main__":
    main()
