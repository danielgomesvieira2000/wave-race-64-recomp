"""Fixes the pinned dependencies need to compile on macOS.

**Symptom:** on macOS the RT64 build stops in hlsl++ with

    error: use of undeclared identifier 'labs'

at `_hlslpp_abs_epi32` in `hlsl++/platforms/scalar.h`. That file calls `labs`
but includes only `<math.h>` and `<stdint.h>`; `labs` is declared in
`<stdlib.h>`. On Windows and Linux it compiles anyway because the platform's
`<math.h>` drags `<stdlib.h>` in behind it, which is an accident of those
headers rather than anything hlsl++ asked for. Apple's libc does not, so the
declaration is simply missing.

The scalar path is not the one that normally runs -- hlsl++ prefers SSE or NEON
-- but the header is parsed regardless of which path is selected, so the error
is unconditional.

Scripted and idempotent because it patches a submodule: a submodule update would
otherwise revert it silently. Safe to run on any platform, and run by
`tools/patch_rt64.py` along with the rest.

Run from the repository root:
    python tools/patch_macos.py
"""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SCALAR = REPO / "lib" / "RT64" / "src" / "contrib" / "hlslpp" / "include" / "hlsl++" / "platforms" / "scalar.h"

ANCHOR = "#include <stdint.h>"
ADDITION = "#include <stdlib.h> // wr64: labs, which this file calls and Apple's math.h does not declare"


def main() -> int:
    if not SCALAR.is_file():
        print(f"not found: {SCALAR}\n"
              "Run: git submodule update --init --recursive", file=sys.stderr)
        return 1

    text = SCALAR.read_text(encoding="utf-8")

    if ADDITION in text:
        print("hlsl++ scalar.h: already patched")
        return 0

    if ANCHOR not in text:
        print(f"anchor not found in {SCALAR.name}: {ANCHOR!r}\n"
              "The submodule has moved; re-derive this patch before continuing.",
              file=sys.stderr)
        return 1

    SCALAR.write_text(text.replace(ANCHOR, ANCHOR + "\n" + ADDITION, 1), encoding="utf-8")
    print("hlsl++ scalar.h: declared labs with stdlib.h")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
