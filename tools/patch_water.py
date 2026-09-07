#!/usr/bin/env python3
"""Apply the optional water renderer after patch_rt64.py and patch_macos.py.

The patch records only water changes, against the project's existing patched
RT64. Refuse partial/mismatched trees instead of replacing local source files.
"""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
LIBRARY = ROOT / "lib/RT64"
PATCH = ROOT / "tools/patches/rt64-water.patch"


def check(*args):
    return subprocess.run(["git", "apply", "--check", *args, str(PATCH)],
                          cwd=LIBRARY, capture_output=True, text=True)


def main():
    if check("--reverse").returncode == 0:
        print("RT64 water renderer: already applied")
        return 0
    forward = check()
    if forward.returncode:
        print("RT64 water patch does not match this checkout. Preserve local changes and inspect the conflicting hunks:", file=sys.stderr)
        print(forward.stderr, file=sys.stderr)
        return 1
    subprocess.run(["git", "apply", str(PATCH)], cwd=LIBRARY, check=True)
    print("RT64 water renderer: applied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
