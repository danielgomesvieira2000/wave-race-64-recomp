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


def apply_patch(submodule: Path, patch: Path, name: str) -> int:
    """Apply a patch to a submodule, idempotently. Shared with the runtime one."""
    if not (submodule / ".git").exists() and not (submodule / "CMakeLists.txt").is_file():
        print(f"{name}: {submodule} is missing.\n"
              "Run: git submodule update --init --recursive", file=sys.stderr)
        return 1
    if not patch.is_file():
        print(f"{name}: {patch} is missing.", file=sys.stderr)
        return 1

    def check(*args):
        return subprocess.run(["git", "apply", "--check", *args, str(patch)],
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

    subprocess.run(["git", "apply", str(patch)], cwd=submodule, check=True)
    print(f"{name}: applied")
    return 0


def main() -> int:
    return apply_patch(SUBMODULE, PATCH, NAME)


if __name__ == "__main__":
    raise SystemExit(main())
