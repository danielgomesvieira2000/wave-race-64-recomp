"""The patches to RT64 this port needs, listed below.

The first two, which widescreen in Wave Race 64 needs, come from the same fact about this game: it does not draw to its whole
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

5. There is no way to see which pair was made.

   The counters above say how many transforms went unpaired, which costs
   nothing; the defects are wrong pairs. WR64_PAIRING_LOG names a file, and RT64
   writes into it, per frame, every world transform's pair with its call hash,
   path and jump, and every camera's pair with its framebuffer and screen
   region. tools/pairing_log.py reads it. See docs/TRANSFORM-PAIRING.md.

6. A split-screen view is drawn through the other view's camera.

   RT64 pairs each frame's cameras with the previous frame's by matrix
   difference alone. In a 2P VS start the two views film the same riders while
   the intro camera sweeps, the other view's camera is often the closer one,
   and for over a second each view flickered between two shots on every generated
   frame. A camera now only continues a previous camera on the same
   framebuffer slot that drew into mostly the same part of the screen.
   WR64_NO_SCENE_REGIONS=1 switches it off.

7. A pair no object could have made is interpolated anyway.

   RT64 accepts any candidate pair whatever the distance, so a buoy took one
   400 to 10,000 units away. A candidate further apart than 150 world units is
   refused before it is scored; nothing in this game moves more than about 60
   between frames. WR64_PAIRING_MAX_JUMP sets the limit; 0 switches it off.

Scripted and idempotent because they patch a submodule: a submodule update
would otherwise revert them silently.

Run from the repository root:
    python tools/patch_rt64.py
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

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

# ---- the pairing log, and the jump limit -----------------------------------
#
# The counters above say how many transforms found no pair. The log says which
# pair each one found, which is the number that matters: a transform paired
# with the wrong previous transform is interpolated from wherever that other
# object was, and a part of a moving model left unpaired is drawn apart from
# the rest. WR64_PAIRING_LOG names a file; nothing is written without it.
#
# One line per transform per interpolated frame:
#
#   F <frame> wall=<ms>                        a new game frame
#   V <persp|ortho> cur=<i>/<n> fb=<slot> n=<projections> scissor=<x0,y0,x1,y1>
#       prev=<j>/<n> fb=<slot> n=<projections> scissor=<x0,y0,x1,y1> diff=<d>
#   T <t> <prev> <path> id=<id> call=<hash> range=<min>-<max> cur=<x,y,z>
#       prev=<x,y,z> jump=<units> lerp=<0|1> pvel=<units>
#   U <t> id=<id> call=<hash> range=<min>-<max> cur=<x,y,z>
#   S frame=<n> total=<n> paired=<n> j50=<n> j100=<n> j200=<n> max=<units>
#       refused=<n>
#
# <wall> is milliseconds since the Unix epoch, to line a frame up with a
# capture (tools/capture_frames.py writes the launch time in the same units).
# A V line is one camera pairing as matchScenes decided it: scene i of this
# frame's n with scene j of the previous frame's, the framebuffer slot of each
# scene's first projection, the union of its projections' scissors in quarter
# pixels (INT_MAX,INT_MAX,INT_MIN,INT_MIN for a scene that drew nothing), and
# RT64's matrix difference between the two cameras.
#
# <path> is "id" for a transform paired by explicit id (G_EX_ORDER_LINEAR) and
# "auto" for one paired by the call-hash heuristic. <call> is RT64's own hash
# of the draw call the transform sits in, and <range> the world-matrix indices
# that call spans -- two transforms with the same call hash and count are what
# the heuristic pairs by index. <jump> is the distance between the previous
# and current translations, <lerp> whether the rigid body agreed to move the
# object between them, and <pvel> the previous frame's velocity for that
# object, which is what the rigid body judged the jump against. <refused> is
# the number of candidates the jump limit below turned away this frame.
#
# The jump limit. RT64's matcher has no notion of an impossible pair: any
# candidate that is not a mirror image is accepted, and a 500-unit jump is
# interpolated as motion. Nothing in this game moves more than a few tens of
# units per game frame (measured with the log above), so a candidate further
# away than WR64_PAIRING_MAX_JUMP world units is refused before it can be
# scored; the transform is then drawn at its current matrix if nothing else
# claims it, which is a one-frame step rather than a slide. The environment
# overrides the built-in default for experiments; 0 switches the limit off.

# The built-in jump limit, in world units, as a C float literal. Set from the
# measurement in docs/TRANSFORM-PAIRING.md: nothing in a race moves more than
# about 60 units between two game frames (largest seen in a 2P start: 63), and
# the course's repeated objects stand 400 apart. "0.0f" switches it off.
PAIRING_MAX_JUMP_DEFAULT = "150.0f"

PAIRING_LOG_INCLUDE_ANCHOR = """// wr64: for the transform-pairing counters below.
#include <unordered_set>
"""

PAIRING_LOG_INCLUDE_REPLACEMENT = """// wr64: for the transform-pairing counters below.
#include <unordered_set>

// wr64: for the pairing log and the jump limit.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
"""

# Inserted immediately before the counters block above, not inside or after
# it. Inside would break the counters patch: patch() decides "already applied"
# by looking for its whole replacement, so a later patch that edits text inside
# an earlier replacement makes the earlier one apply again on the next run.
# After would break the water renderer's patch, whose first hunk in this file
# has "namespace RT64 { // GameFrame" and the blank line after it as context.
PAIRING_LOG_GLOBALS_ANCHOR = """// wr64: running totals of the transform pairing, read by the port through the
"""

PAIRING_LOG_GLOBALS_BEGIN = "// wr64-pairing-globals-begin\n"
PAIRING_LOG_GLOBALS_END = "// wr64-pairing-globals-end\n"

PAIRING_LOG_GLOBALS_REGION = """// wr64-pairing-globals-begin
#include <chrono>
// wr64: the pairing log (WR64_PAIRING_LOG names a file) and the jump limit
// (WR64_PAIRING_MAX_JUMP, world units; 0 switches it off). Both are read once.
static FILE *wr64PairingLog() {
    static FILE *file = [] {
        const char *path = std::getenv("WR64_PAIRING_LOG");
        return (path != nullptr && path[0] != '\\0') ? std::fopen(path, "w") : nullptr;
    }();
    return file;
}

static float wr64PairingMaxJump() {
    static const float value = [] {
        const char *text = std::getenv("WR64_PAIRING_MAX_JUMP");
        return (text != nullptr && text[0] != '\\0') ? float(std::atof(text)) : WR64_PAIRING_MAX_JUMP_DEFAULT;
    }();
    return value;
}

static uint64_t wr64PairingRefused = 0;

// wr64: the scene matches of the current frame, for the pairing log. Filled by
// matchScenes only while the log is open, and emptied as it is written.
struct Wr64SceneMatch { uint32_t curIndex; uint32_t prevIndex; float difference; bool perspective; };
static thread_local std::vector<Wr64SceneMatch> wr64SceneMatches;
// wr64-pairing-globals-end
"""

PAIRING_JUMP_ANCHOR ="""        // Compute the difference between the translation components of the 4x4 matrices.
        const hlslpp::float3 curPos = curTransform[3].xyz;
        hlslpp::float3 prevPos = prevTransform[3].xyz;
"""

PAIRING_JUMP_REPLACEMENT = """        // Compute the difference between the translation components of the 4x4 matrices.
        const hlslpp::float3 curPos = curTransform[3].xyz;
        hlslpp::float3 prevPos = prevTransform[3].xyz;

        // wr64: refuse a pair no object could have made. The distance is taken
        // between the raw translations, before the velocity prediction below,
        // so that a velocity polluted by an earlier wrong pair cannot make a
        // second wrong pair look plausible.
        {
            const float maxJump = wr64PairingMaxJump();
            if (maxJump > 0.0f) {
                const float rawJump = hlslpp::length(curPos - prevPos);
                if (rawJump > maxJump) {
                    wr64PairingRefused++;
                    return matchResult;
                }
            }
        }
"""

PAIRING_LOG_ANCHOR = """            wr64PairingFrames++;
            wr64PairingTotal += total;
            wr64PairingUnpaired += unpaired;
            wr64PairingUnpairedMoved += unpairedMoved;
        }
"""

# The log block goes after the counters, delimited by markers and replaced in
# place by patch_region(), because its text changes as the log grows: an
# anchored patch whose replacement changed would be inserted a second time.
PAIRING_LOG_BEGIN = "        // wr64-pairing-log-begin\n"
PAIRING_LOG_END = "        // wr64-pairing-log-end\n"

PAIRING_LOG_REGION = """        // wr64-pairing-log-begin
        // wr64: the pairing log. See tools/patch_rt64.py for the format.
        if (FILE *log = wr64PairingLog()) {
            static uint64_t logFrame = 0;
            static uint64_t refusedBefore = 0;
            logFrame++;
            const long long wallMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            std::fprintf(log, "F %llu wall=%lld\\n", (unsigned long long)logFrame, wallMs);

            // Which previous scene each current scene was matched with, as
            // matchScenes decided it (recorded there), with the part of the
            // screen each drew into. A scene is one camera on one framebuffer;
            // a split-screen view matched with the other view's camera is drawn
            // through the wrong camera on every generated frame.
            auto sceneBounds = [&](const GameScene &scene, uint32_t &fbPairIndex) {
                FixedRect bounds;
                fbPairIndex = scene.projections.empty() ? UINT32_MAX : scene.projections[0].fbPairIndex;
                for (const GameIndices::Projection &pi : scene.projections) {
                    const Projection &pp = workloadQueue.workloads[pi.workloadIndex].fbPairs[pi.fbPairIndex].projections[pi.projectionIndex];
                    if (!pp.scissorRect.isNull()) {
                        bounds.merge(pp.scissorRect);
                    }
                }
                return bounds;
            };

            for (const Wr64SceneMatch &m : wr64SceneMatches) {
                const std::vector<GameScene> &cur = m.perspective ? perspectiveScenes : orthographicScenes;
                const std::vector<GameScene> &prev = m.perspective ? prevFrame.perspectiveScenes : prevFrame.orthographicScenes;
                uint32_t curFb = 0, prevFb = 0;
                const FixedRect cb = sceneBounds(cur[m.curIndex], curFb);
                const FixedRect pb = sceneBounds(prev[m.prevIndex], prevFb);
                std::fprintf(log, "V %s cur=%u/%zu fb=%u n=%zu scissor=%d,%d,%d,%d prev=%u/%zu fb=%u n=%zu scissor=%d,%d,%d,%d diff=%.2f\\n",
                             m.perspective ? "persp" : "ortho", m.curIndex, cur.size(), curFb, cur[m.curIndex].projections.size(),
                             cb.ulx, cb.uly, cb.lrx, cb.lry, m.prevIndex, prev.size(), prevFb, prev[m.prevIndex].projections.size(),
                             pb.ulx, pb.uly, pb.lrx, pb.lry, m.difference);
            }
            wr64SceneMatches.clear();

            struct CallContext { uint64_t hash; uint32_t min; uint32_t max; };
            thread_local std::vector<CallContext> context;
            uint32_t total = 0, paired = 0, over50 = 0, over100 = 0, over200 = 0;
            float maxJump = 0.0f;
            for (uint32_t w : workloads) {
                const GameFrameMap::WorkloadMap &map = frameMap.workloads[w];
                const Workload &curWorkload = workloadQueue.workloads[w];
                const Workload *prevWorkload = map.mapped ? &workloadQueue.workloads[map.prevWorkloadIndex] : nullptr;
                const GameFrameMap::WorkloadMap *prevMap = nullptr;
                if (map.mapped && prevFrame.matched && prevFrame.frameMap.workloads[map.prevWorkloadIndex].mapped) {
                    prevMap = &prevFrame.frameMap.workloads[map.prevWorkloadIndex];
                }

                // The call each transform sits in, hashed as the matcher hashes it.
                const size_t transformCount = curWorkload.drawData.worldTransforms.size();
                context.assign(transformCount, CallContext{ 0, 0, 0 });
                for (uint32_t f = 0; f < curWorkload.fbPairCount; f++) {
                    const FramebufferPair &fbPair = curWorkload.fbPairs[f];
                    for (uint32_t p = 0; p < fbPair.projectionCount; p++) {
                        const Projection &proj = fbPair.projections[p];
                        for (uint32_t c = 0; c < proj.gameCallCount; c++) {
                            const GameCall &call = proj.gameCalls[c];
                            const uint32_t minMatrix = call.callDesc.minWorldMatrix;
                            const uint32_t maxMatrix = call.callDesc.maxWorldMatrix;
                            if (minMatrix > maxMatrix) {
                                continue;
                            }

                            uint32_t matrixIdHash = 0;
                            for (uint32_t m = minMatrix; m <= maxMatrix && m < transformCount; m++) {
                                const uint32_t groupIndex = curWorkload.drawData.worldTransformGroups[m];
                                matrixIdHash = matrixIdHash * 33 ^ curWorkload.drawData.transformGroups[groupIndex].matrixId;
                            }

                            const uint64_t hash = hashFromCall(call, matrixIdHash);
                            for (uint32_t m = minMatrix; m <= maxMatrix && m < transformCount; m++) {
                                context[m] = CallContext{ hash, minMatrix, maxMatrix };
                            }
                        }
                    }
                }

                for (size_t t = 0; t < map.transforms.size() && t < transformCount; t++) {
                    total++;
                    float cur[16];
                    std::memcpy(cur, &curWorkload.drawData.worldTransforms[t], sizeof(cur));
                    const uint32_t groupIndex = curWorkload.drawData.worldTransformGroups[t];
                    const TransformGroup &group = curWorkload.drawData.transformGroups[groupIndex];
                    const CallContext &ctx = context[t];
                    if (!map.transforms[t].mapped || prevWorkload == nullptr) {
                        std::fprintf(log, "U %zu id=%08X call=%016llX range=%u-%u cur=%.1f,%.1f,%.1f\\n",
                                     t, group.matrixId, (unsigned long long)ctx.hash, ctx.min, ctx.max,
                                     cur[12], cur[13], cur[14]);
                        continue;
                    }

                    paired++;
                    const uint32_t prevIndex = map.transforms[t].prevTransformIndex;
                    float prev[16] = {};
                    if (prevIndex < prevWorkload->drawData.worldTransforms.size()) {
                        std::memcpy(prev, &prevWorkload->drawData.worldTransforms[prevIndex], sizeof(prev));
                    }

                    const float dx = cur[12] - prev[12], dy = cur[13] - prev[13], dz = cur[14] - prev[14];
                    const float jump = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (jump > 50.0f) over50++;
                    if (jump > 100.0f) over100++;
                    if (jump > 200.0f) over200++;
                    if (jump > maxJump) maxJump = jump;

                    float prevVelocity = 0.0f;
                    if (prevMap != nullptr && prevIndex < prevMap->transforms.size()) {
                        const hlslpp::float3 &v = prevMap->transforms[prevIndex].rigidBody.linearVelocity;
                        prevVelocity = hlslpp::length(v);
                    }

                    const bool byId = (group.matrixId != G_EX_ID_AUTO) && (group.matrixId != G_EX_ID_IGNORE) && (group.ordering == G_EX_ORDER_LINEAR);
                    std::fprintf(log, "T %zu %u %s id=%08X call=%016llX range=%u-%u cur=%.1f,%.1f,%.1f prev=%.1f,%.1f,%.1f jump=%.1f lerp=%d pvel=%.1f\\n",
                                 t, prevIndex, byId ? "id" : "auto", group.matrixId, (unsigned long long)ctx.hash,
                                 ctx.min, ctx.max, cur[12], cur[13], cur[14], prev[12], prev[13], prev[14],
                                 jump, map.transforms[t].rigidBody.lerpTranslation ? 1 : 0, prevVelocity);
                }
            }

            std::fprintf(log, "S frame=%llu total=%u paired=%u j50=%u j100=%u j200=%u max=%.1f refused=%llu\\n",
                         (unsigned long long)logFrame, total, paired, over50, over100, over200, maxJump,
                         (unsigned long long)(wr64PairingRefused - refusedBefore));
            refusedBefore = wr64PairingRefused;
            std::fflush(log);
        }
        // wr64-pairing-log-end
"""


# ---- scene matching by screen region --------------------------------------
#
# RT64 pairs each camera ("scene": one view and projection on one framebuffer)
# of a frame with one of the previous frame's by matrix difference alone, then
# interpolates the camera between the two and pairs the objects drawn under it.
# In a split-screen race the two views' cameras film the same riders from
# nearby, and while the intro camera sweeps, the other view's previous camera is
# often closer than the view's own. Measured with the pairing log in a 2P VS
# start: for 33 consecutive game frames the top view took the bottom view's
# camera, the bottom view took an empty scene on another framebuffer, and that
# took the top view's. Every generated frame then drew each view through the
# wrong camera, and the views flickered between two shots for a second -- the
# "burst" at the start of a two-player race.
#
# A camera that draws into a different framebuffer, or a different part of the
# screen, is a different camera whatever its matrices. So a candidate pair is
# kept only if both scenes' first projections use the same framebuffer slot of
# the frame with the same format, and the screen regions their draw calls cover
# overlap by at least half of the smaller one (two scenes that draw nothing are
# alike). Among the pairs that remain, the smallest matrix difference still
# wins. The framebuffer's address is deliberately not compared: the game
# alternates between two framebuffers every frame. WR64_NO_SCENE_REGIONS=1
# switches this off.

SCENE_REGION_HELPER_ANCHOR = """        auto matchScenes = [&](const std::vector<GameScene> &curScenes, const std::vector<GameScene> &prevScenes) {
"""

SCENE_REGION_HELPER_REPLACEMENT = """        // wr64: a scene may only continue a previous scene that draws to the same
        // framebuffer slot and mostly the same part of the screen. See
        // tools/patch_rt64.py, "scene matching by screen region".
        static const bool wr64SceneRegionsOff = []() {
            const char *value = std::getenv("WR64_NO_SCENE_REGIONS");
            return (value != nullptr) && (value[0] != '\\0') && (value[0] != '0');
        }();

        auto wr64SceneRegion = [&](const GameScene &scene) {
            FixedRect region;
            for (const GameIndices::Projection &pi : scene.projections) {
                const Projection &pp = workloadQueue.workloads[pi.workloadIndex].fbPairs[pi.fbPairIndex].projections[pi.projectionIndex];
                if (!pp.scissorRect.isNull()) {
                    region.merge(pp.scissorRect);
                }
            }

            return region;
        };

        auto wr64ScenesCompatible = [&](const GameScene &cur, const GameScene &prev) {
            if (wr64SceneRegionsOff) {
                return true;
            }

            const GameIndices::Projection &ci = cur.projections[0];
            const GameIndices::Projection &pi = prev.projections[0];
            if (ci.fbPairIndex != pi.fbPairIndex) {
                return false;
            }

            const auto &cc = workloadQueue.workloads[ci.workloadIndex].fbPairs[ci.fbPairIndex].colorImage;
            const auto &pc = workloadQueue.workloads[pi.workloadIndex].fbPairs[pi.fbPairIndex].colorImage;
            if ((cc.fmt != pc.fmt) || (cc.siz != pc.siz) || (cc.width != pc.width)) {
                return false;
            }

            const FixedRect cr = wr64SceneRegion(cur);
            const FixedRect pr = wr64SceneRegion(prev);
            if (cr.isNull() || pr.isNull()) {
                return cr.isNull() && pr.isNull();
            }

            const FixedRect overlap = cr.intersection(pr);
            if (overlap.isNull()) {
                return false;
            }

            auto area = [](const FixedRect &r) { return int64_t(r.lrx - r.ulx) * int64_t(r.lry - r.uly); };
            return (2 * area(overlap)) >= std::min(area(cr), area(pr));
        };

        auto matchScenes = [&](const std::vector<GameScene> &curScenes, const std::vector<GameScene> &prevScenes) {
"""

SCENE_REGION_FILTER_ANCHOR = """                    matchCandidates.emplace_back(i, j, matrixDifference(curViewTransform, prevViewTransform) + matrixDifference(curProjTransform, prevProjTransform));
"""

SCENE_REGION_FILTER_REPLACEMENT = """                    // wr64: see wr64ScenesCompatible above.
                    if (!wr64ScenesCompatible(curScenes[i], prevScenes[j])) {
                        continue;
                    }

                    matchCandidates.emplace_back(i, j, matrixDifference(curViewTransform, prevViewTransform) + matrixDifference(curProjTransform, prevProjTransform));
"""

PAIRING_SCENE_ANCHOR = """                matchScene(workloadQueue, prevFrame, curScenes[candidate.curIndex], prevScenes[candidate.prevIndex], workloadsModified, tileInterpolationUsed, lookAtInterpolationUsed);
"""

PAIRING_SCENE_REPLACEMENT = """                // wr64: record the decision for the pairing log.
                if (wr64PairingLog() != nullptr) {
                    wr64SceneMatches.push_back({ candidate.curIndex, candidate.prevIndex, candidate.difference, &curScenes == &perspectiveScenes });
                }

                matchScene(workloadQueue, prevFrame, curScenes[candidate.curIndex], prevScenes[candidate.prevIndex], workloadsModified, tileInterpolationUsed, lookAtInterpolationUsed);
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


def patch_region(target, anchor, begin, end, region, name, after=False):
    """Like patch(), for a block whose text may change between runs: the block is
    delimited by marker lines, inserted before the anchor (after it, with
    after=True) the first time and replaced in place every time after, so a
    changed block never duplicates."""
    text = target.read_text()
    if region in text:
        print(f"  {target.name}: {name} already patched")
        return
    start = text.find(begin)
    if start >= 0:
        stop = text.find(end, start)
        if stop < 0:
            sys.exit(f"{name}: begin marker without end marker in {target}")
        text = text[:start] + region + text[stop + len(end):]
        print(f"  {target.name}: {name} re-patched")
    else:
        if anchor not in text:
            sys.exit(f"anchor for {name} not found in {target}; upstream has changed "
                     f"and this patch needs revisiting")
        text = text.replace(anchor, (anchor + region) if after else (region + anchor), 1)
        print(f"  {target.name}: {name} patched")
    target.write_text(text)


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
    patch(GAME_FRAME, PAIRING_LOG_INCLUDE_ANCHOR, PAIRING_LOG_INCLUDE_REPLACEMENT, "pairing log (include)")
    patch_region(GAME_FRAME, PAIRING_LOG_GLOBALS_ANCHOR, PAIRING_LOG_GLOBALS_BEGIN, PAIRING_LOG_GLOBALS_END,
                 PAIRING_LOG_GLOBALS_REGION.replace("WR64_PAIRING_MAX_JUMP_DEFAULT", PAIRING_MAX_JUMP_DEFAULT),
                 "pairing log (globals)")
    patch(GAME_FRAME, PAIRING_JUMP_ANCHOR, PAIRING_JUMP_REPLACEMENT, "pairing jump limit")
    patch(GAME_FRAME, PAIRING_SCENE_ANCHOR, PAIRING_SCENE_REPLACEMENT, "pairing log (scene matches)")
    patch(GAME_FRAME, SCENE_REGION_HELPER_ANCHOR, SCENE_REGION_HELPER_REPLACEMENT, "scene regions (helpers)")
    patch(GAME_FRAME, SCENE_REGION_FILTER_ANCHOR, SCENE_REGION_FILTER_REPLACEMENT, "scene regions (filter)")
    patch_region(GAME_FRAME, PAIRING_LOG_ANCHOR, PAIRING_LOG_BEGIN, PAIRING_LOG_END, PAIRING_LOG_REGION,
                 "pairing log (write)", after=True)

    # The inspector hook lives in its own script because it answers a different
    # question, but the port links against the symbol it adds, so a build needs
    # it as much as the patches above. One command applies everything required.
    import patch_rt64_inspector
    patch_rt64_inspector.main()

    # The texture-pack setter, which mods are loaded through. It anchors on the
    # inspector hook above, so it has to follow it.
    import patch_rt64_texturepacks
    patch_rt64_texturepacks.main()

    # Taking RT64's SDL event filter back off when it shuts down. Not a feature
    # the port adds but a missing half of a pairing upstream, and without it the
    # process dies every time the game is closed.
    import patch_rt64_eventfilter
    patch_rt64_eventfilter.main()

    # A missing include in the pinned hlsl++ that only macOS notices. Applied
    # on every platform so there is one patch step to document and one tree
    # state to reason about, whichever machine the build is on.
    import patch_macos
    patch_macos.main()

    # The modern water renderer. Last, because it is the largest and touches
    # several of the files the patches above do -- applying it first would make
    # their anchors harder to find, not the other way round. Water quality
    # Original is a true bypass, so this costs a player who does not want it
    # nothing, but the port does not compile without the headers it adds.
    import patch_rt64_water
    if patch_rt64_water.main() != 0:
        raise SystemExit(1)

    print("Rebuild to pick it up.")


if __name__ == "__main__":
    main()
