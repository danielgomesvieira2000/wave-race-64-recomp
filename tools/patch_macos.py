#!/usr/bin/env python3
"""Idempotent fixes needed by the pinned libraries on macOS."""

from pathlib import Path

root = Path(__file__).resolve().parent.parent
header = root / "lib/RT64/src/contrib/hlslpp/include/hlsl++/platforms/scalar.h"
text = header.read_text()
if "#include <stdlib.h>" not in text:
    anchor = "#include <stdint.h>"
    if anchor not in text:
        raise SystemExit(f"Missing patch anchor in {header}")
    header.write_text(text.replace(anchor, anchor + "\n#include <stdlib.h>", 1))
    print("hlsl++ scalar math: declared labs with stdlib.h")
