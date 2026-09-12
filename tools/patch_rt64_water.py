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


def main() -> int:
    # The guard below edits a line the diff itself introduced, so once it is in,
    # "git apply --reverse --check" no longer recognises the tree as patched.
    # The guard's marker is therefore the authority on "fully applied", and it
    # is checked before anything else is attempted.
    if FB_RENDERER.is_file() and GUARD_MARKER in FB_RENDERER.read_text(encoding="utf-8"):
        print(f"{NAME}: already applied")
        return 0

    result = apply_patch(SUBMODULE, PATCH, NAME)
    if result != 0:
        return result
    return apply_guard()


if __name__ == "__main__":
    raise SystemExit(main())
