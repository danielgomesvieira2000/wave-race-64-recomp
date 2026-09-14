"""Add the modern water renderer to RT64.

This is the one patch in `tools/` that is a diff file rather than a set of
anchored string replacements, and the reason is size: it adds a renderer
(`src/render/rt64_water_renderer.{cpp,h}`), an interpolation header, a shared
parameter block and ten HLSL shaders, and threads them through twenty existing
RT64 files -- about 1200 lines of edits. Transcribing that into anchors would
add a class of error the diff does not have, and buy nothing: `git apply`
already refuses a tree it does not match, which is the property the anchored
scripts exist to provide.

It is idempotent the same way. `git apply --reverse --check` succeeds only when
the patch is already fully applied, so a second run is a no-op and a partially
applied tree is reported rather than half-patched again.

What it does, in short. The game builds its water surface as a lattice of
vertices that it recenters around the camera every frame, and RT64 sees only the
resulting display list. The renderer recognises the four USA Rev A water display
lists, takes the surface it would have drawn, and shades it with sun and sky
lighting, depth-dependent colour, refraction, wakes and shoreline wash. The
game's own wave simulation and physics are untouched: the *heights* are still
the cartridge's, and everything added is shading and detail on top of them.

See docs/WATER.md for what it costs and what each setting does. `Original` is
the default, and on that setting the renderer is not engaged at all.

Scripted because it patches a submodule: a submodule update would otherwise
revert it silently, and the failure -- a port that no longer compiles because
`shared/rt64_water_params.h` has gone -- points nowhere near its cause.

Run from the repository root:
    python tools/patch_rt64_water.py
"""

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SUBMODULE = REPO / "lib" / "RT64"
PATCH = REPO / "tools" / "patches" / "rt64-water.patch"
NAME = "RT64 water renderer"


# git apply is line-ending sensitive, and this tree is checked out by Windows
# git with core.autocrlf=true while the patches are stored LF. Windows git
# normalises during apply and does not notice; WSL's git has no autocrlf, sees
# CRLF content against LF context, and rejects the patch in *both* directions --
# so the reverse-check idempotency test reports "does not match this checkout"
# for a patch that is already fully applied.
#
# --ignore-whitespace is the documented tolerance for exactly that: with it the
# reverse check correctly succeeds on an applied tree and the forward check
# still correctly refuses to apply twice.
IGNORE_WS = "--ignore-whitespace"


def apply_patch(submodule: Path, patch: Path, name: str, marker: tuple = None) -> int:
    """Apply a patch to a submodule, idempotently. Shared with the runtime one.

    `marker` is an optional (path, string) pair that settles "already applied"
    without invoking git at all, which is both cheaper and immune to the
    line-ending problem described above.
    """
    if marker is not None:
        marker_path, marker_text = marker
        if marker_path.is_file() and marker_text in marker_path.read_text(encoding="utf-8"):
            print(f"{name}: already applied")
            return 0
    if not (submodule / ".git").exists() and not (submodule / "CMakeLists.txt").is_file():
        print(f"{name}: {submodule} is missing.\n"
              "Run: git submodule update --init --recursive", file=sys.stderr)
        return 1
    if not patch.is_file():
        print(f"{name}: {patch} is missing.", file=sys.stderr)
        return 1

    def check(*args):
        return subprocess.run(["git", "apply", "--check", IGNORE_WS, *args, str(patch)],
                              cwd=submodule, capture_output=True, text=True)

    # Reverse-applies cleanly means it is already there, in full.
    if check("--reverse").returncode == 0:
        print(f"{name}: already applied")
        return 0

    forward = check()
    if forward.returncode != 0:
        print(f"{name}: this patch does not match the checkout in {submodule.name}.\n"
              "Nothing has been changed. Either the submodule has moved, or another\n"
              "patch has already touched the same lines -- look at the hunks below and\n"
              "re-derive the patch rather than forcing it.\n",
              file=sys.stderr)
        print(forward.stderr, file=sys.stderr)
        return 1

    subprocess.run(["git", "apply", IGNORE_WS, str(patch)], cwd=submodule, check=True)
    print(f"{name}: applied")
    return 0


# ---------------------------------------------------------------------------
#  A precondition the renderer assumes and does not check
#
#  **Symptom:** with Water set to anything but Original the port dies on the
#  title screen, reading address 0x44 in FramebufferRenderer::addFramebuffer,
#  on the graphics thread, under State::fullSync.
#
#  addFramebuffer indexes drawData.modViewTransforms[proj.transformsIndex] to
#  get the camera for the water pass. Those "mod" transforms are not part of a
#  workload: ProjectionProcessor::process fills them, for the workloads in the
#  frame it is given, by copying the raw transforms. Reached through fullSync
#  instead of the normal present path, that processing has not run, the vector
#  is empty, and the index walks off the front of it.
#
#  The fix is the precondition, not a fallback: if the modified transforms for
#  this projection are not there, the draw is not one this renderer can take
#  over, and it falls through to the game's own water for that frame -- which
#  is what the renderer already does for any display list it does not
#  recognise.
#
#  Anchored rather than folded into the diff above because it is three lines
#  and belongs to a symptom worth keeping next to its explanation.
# ---------------------------------------------------------------------------

FB_RENDERER = SUBMODULE / "src" / "render" / "rt64_framebuffer_renderer.cpp"

GUARD_MARKER = "wr64: the modified transforms may not exist"

GUARD_ANCHOR = """                const bool wantsWater = call.callDesc.waterMaterial.identity.x > 0.0f &&
                    instanceDrawCall.type == InstanceDrawCall::Type::IndexedTriangles && p.fbStorage->colorTarget && p.fbStorage->depthTarget;
"""

GUARD_REPLACEMENT = """                // wr64: the modified transforms may not exist for this projection.
                // ProjectionProcessor::process fills them per frame; a framebuffer
                // added through State::fullSync has not been through it, and the
                // index below would read off the front of an empty vector.
                const bool waterTransformsReady =
                    proj.transformsIndex < drawData.modViewTransforms.size() &&
                    proj.transformsIndex < drawData.modViewProjTransforms.size();
                const bool wantsWater = call.callDesc.waterMaterial.identity.x > 0.0f &&
                    instanceDrawCall.type == InstanceDrawCall::Type::IndexedTriangles && p.fbStorage->colorTarget && p.fbStorage->depthTarget &&
                    waterTransformsReady;
"""


def apply_guard() -> int:
    text = FB_RENDERER.read_text(encoding="utf-8")
    if GUARD_MARKER in text:
        print(f"{NAME}: transform guard already applied")
        return 0
    if GUARD_ANCHOR not in text:
        print(f"{NAME}: could not find the wantsWater anchor in {FB_RENDERER.name}.\n"
              "The water patch has moved; re-derive this guard before continuing.",
              file=sys.stderr)
        return 1
    FB_RENDERER.write_text(text.replace(GUARD_ANCHOR, GUARD_REPLACEMENT, 1), encoding="utf-8")
    print(f"{NAME}: transform guard applied")
    return 0


# ---------------------------------------------------------------------------
#  The same interpolation at Original
#
#  Not the cause of the jittering water reported against 1.0.0 -- that was the
#  sea extension ring sharing the water's transform; see src/dlrewrite.cpp,
#  water_ring_transform, and docs/PORTING.md. This makes Original water
#  interpolate the way Enhanced and Best already did.
#
#  The game carries its water lattice with the camera in 64-unit steps, so
#  vertex n is not the same water from one game frame to the next, and RT64's
#  own vertex interpolation, which pairs by index, slides the whole surface up
#  to 128 units across the generated frames. The diff replaces that with
#  sampling the previous surface at each current world XZ -- but only for a draw
#  whose material has a quality above zero, which is to say only when the modern
#  renderer shades it. At Original the port sent the cleared material, the mesh
#  was never tagged, and the water fell back to pairing by index.
#
#  The history that sampling needs -- course, visual generation, the two
#  animation times, the viewport -- has nothing to do with shading. The port now
#  sends its snapshot at Original too, with quality zero so the renderer stays
#  out of the draw, and the tag below accepts it: identity.w, the port's visual
#  generation, is at least one in every snapshot and zero only in the cleared
#  command.
#
#  WR64_WATER_INTERP_STATS=1 prints, every 300 water meshes matched, how many
#  were sampled by world XZ, how many vertices found a previous surface, and the
#  CPU time the sampling took on RT64's workload thread.
# ---------------------------------------------------------------------------

GAME_FRAME = SUBMODULE / "src" / "hle" / "rt64_game_frame.cpp"

ORIGINAL_MARKER = "wr64: a snapshot at Original tags the mesh too"

ORIGINAL_EDITS = [
    (
        """#include "rt64_water_interpolation.h"
""",
        """#include "rt64_water_interpolation.h"
// wr64: for WR64_WATER_INTERP_STATS.
#include <chrono>
""",
    ),
    (
        """                        if (transformIndex < desc.minWorldMatrix || transformIndex > desc.maxWorldMatrix || desc.waterMaterial.identity.x <= 0) continue;
""",
        """                        // wr64: a snapshot at Original tags the mesh too. identity.x is
                        // the quality, and zero keeps the renderer out of the draw, but
                        // the history below is as valid then; identity.w, the port's
                        // visual generation, is zero only in the cleared command.
                        if (transformIndex < desc.minWorldMatrix || transformIndex > desc.maxWorldMatrix ||
                            (desc.waterMaterial.identity.x <= 0 && desc.waterMaterial.identity.w <= 0)) continue;
""",
    ),
    (
        """            const WaterMeshContext current = waterMeshContext(curWorkload, curTransformIndex);
            if (current.tagged) {
                float *positionVelocity = curWorkload.drawData.velFloats.data() + curVertexIndex * 3;
                float *texcoordVelocity = curWorkload.drawData.tcVelFloats.data() + curVertexIndex * 2;
                std::fill_n(positionVelocity, curVertexCount * 3, 0.0f);
                std::fill_n(texcoordVelocity, curVertexCount * 2, 0.0f);
                const WaterMeshContext previous = waterMeshContext(prevWorkload, prevTransformIndex);
                if (prevVertexCount > 0 && waterViewsCompatible(curWorkload, current, prevWorkload, previous)) {
                    const WaterInterpolation::PreviousMesh mesh(prevWorkload.drawData.posFloats.data() + prevVertexIndex * 3,
                        prevWorkload.drawData.tcFloats.data() + prevVertexIndex * 2, prevVertexCount, previous.indices);
                    mesh.velocities(curWorkload.drawData.posFloats.data() + curVertexIndex * 3,
                        curWorkload.drawData.tcFloats.data() + curVertexIndex * 2, curVertexCount, positionVelocity, texcoordVelocity);
                }
                // New perimeter points and invalid history keep current data;
                // extrapolating a different patch would invent visible waves.
                modifiedBuffers.positionVelocity = true;
                modifiedBuffers.texcoordVelocity = true;
                return;
            }
""",
        """            static const bool waterStats = std::getenv("WR64_WATER_INTERP_STATS") != nullptr;
            const auto waterStart = waterStats ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            const WaterMeshContext current = waterMeshContext(curWorkload, curTransformIndex);
            // WR64_WATER_INTERP_STATS: every mesh reaching here, the ones sampled by
            // world XZ, their vertices and how many found a previous surface, and the
            // time taken, printed every 300 meshes.
            static uint64_t statMeshes = 0, statSampled = 0, statVertices = 0, statMatched = 0;
            static double statMs = 0, statMaxMs = 0;
            auto waterReport = [&](bool sampled, uint32_t matched) {
                if (!waterStats) return;
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waterStart).count();
                ++statMeshes; statMs += ms; statMaxMs = std::max(statMaxMs, ms);
                if (sampled) { ++statSampled; statVertices += curVertexCount; statMatched += matched; }
                if (statMeshes % 300 == 0) {
                    std::fprintf(stderr, "[water] interpolation: %llu of %llu meshes sampled by world XZ, %.1f%% of %llu vertices matched, %.3f ms mean, %.3f ms max\\n",
                        (unsigned long long)statSampled, (unsigned long long)statMeshes,
                        statVertices ? 100.0 * double(statMatched) / double(statVertices) : 0.0, (unsigned long long)statVertices,
                        statMs / double(statMeshes), statMaxMs);
                    statSampled = statVertices = statMatched = 0; statMs = statMaxMs = 0; statMeshes = 0;
                }
            };
            if (current.tagged) {
                float *positionVelocity = curWorkload.drawData.velFloats.data() + curVertexIndex * 3;
                float *texcoordVelocity = curWorkload.drawData.tcVelFloats.data() + curVertexIndex * 2;
                std::fill_n(positionVelocity, curVertexCount * 3, 0.0f);
                std::fill_n(texcoordVelocity, curVertexCount * 2, 0.0f);
                const WaterMeshContext previous = waterMeshContext(prevWorkload, prevTransformIndex);
                bool sampled = false;
                uint32_t matched = 0;
                if (prevVertexCount > 0 && waterViewsCompatible(curWorkload, current, prevWorkload, previous)) {
                    const WaterInterpolation::PreviousMesh mesh(prevWorkload.drawData.posFloats.data() + prevVertexIndex * 3,
                        prevWorkload.drawData.tcFloats.data() + prevVertexIndex * 2, prevVertexCount, previous.indices);
                    matched = mesh.velocities(curWorkload.drawData.posFloats.data() + curVertexIndex * 3,
                        curWorkload.drawData.tcFloats.data() + curVertexIndex * 2, curVertexCount, positionVelocity, texcoordVelocity);
                    sampled = true;
                }
                waterReport(sampled, matched);
                // New perimeter points and invalid history keep current data;
                // extrapolating a different patch would invent visible waves.
                modifiedBuffers.positionVelocity = true;
                modifiedBuffers.texcoordVelocity = true;
                return;
            }
            // Untagged: left to RT64's pairing by index below.
            waterReport(false, 0);
""",
    ),
]


def apply_edits(path: Path, edits: list, marker: str, what: str) -> int:
    """Anchored replacements, all or nothing, idempotent by `marker`."""
    text = path.read_text(encoding="utf-8")
    if marker in text:
        print(f"{NAME}: {what} already applied")
        return 0
    for anchor, _ in edits:
        if text.count(anchor) != 1:
            print(f"{NAME}: could not find an anchor for {what} in {path.name}.\n"
                  "The water patch has moved; re-derive this edit before continuing.",
                  file=sys.stderr)
            return 1
    for anchor, replacement in edits:
        text = text.replace(anchor, replacement, 1)
    path.write_text(text, encoding="utf-8")
    print(f"{NAME}: {what} applied")
    return 0


def apply_original_interpolation() -> int:
    return apply_edits(GAME_FRAME, ORIGINAL_EDITS, ORIGINAL_MARKER, "interpolation at Original")


def main() -> int:
    # The guard below edits a line the diff itself introduced, so once it is in,
    # "git apply --reverse --check" no longer recognises the tree as patched.
    # The guard's marker is therefore the authority on the diff and the guard
    # having been applied; the edits after it carry markers of their own.
    if FB_RENDERER.is_file() and GUARD_MARKER in FB_RENDERER.read_text(encoding="utf-8"):
        print(f"{NAME}: diff and transform guard already applied")
    else:
        result = apply_patch(SUBMODULE, PATCH, NAME)
        if result == 0:
            result = apply_guard()
        if result != 0:
            return result
    return apply_original_interpolation()


if __name__ == "__main__":
    raise SystemExit(main())
