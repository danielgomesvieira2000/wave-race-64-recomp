#!/usr/bin/env python3
"""Join runtime game and timer workers before releasing RDRAM.

This fixes a shutdown crash exposed by the bounded water replay fixtures.
Refuse partial/mismatched trees instead of replacing local source files.
"""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
LIBRARY = ROOT / "lib/N64ModernRuntime"
PATCH = ROOT / "tools/patches/runtime-shutdown.patch"


def check(*args):
    return subprocess.run(["git", "apply", "--check", *args, str(PATCH)],
                          cwd=LIBRARY, capture_output=True, text=True)


def main():
    if check("--reverse").returncode == 0:
        print("Runtime shutdown: already applied")
        return 0
    forward = check()
    if forward.returncode:
        print("Runtime shutdown patch does not match this checkout. Preserve local changes and inspect the conflicting hunks:", file=sys.stderr)
        print(forward.stderr, file=sys.stderr)
        return 1
    subprocess.run(["git", "apply", str(PATCH)], cwd=LIBRARY, check=True)
    print("Runtime shutdown: applied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
