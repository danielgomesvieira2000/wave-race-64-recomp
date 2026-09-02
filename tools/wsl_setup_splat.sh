#!/usr/bin/env bash
# Phase 01: prepare a python environment for the vendored splat 0.37.1.
#
# We deliberately use the splat vendored in the reference decomp rather than a
# current release from PyPI. The Rev A yaml was written against 0.37.1, and
# splat's config schema has moved on since; matching the version the config was
# written for removes a whole class of "option no longer exists" failures.
set -euo pipefail

VENV="$HOME/wr64venv"
REPO="/mnt/c/Users/Daniel/claude-projects/n64recomp_waverace64"
SPLAT="$REPO/reference/wr64-decomp/tools/splat"

if [ ! -d "$VENV" ]; then
    python3 -m venv "$VENV"
fi

# splat64 from PyPI would shadow the vendored package on import.
"$VENV/bin/pip" uninstall -y -q splat64 2>/dev/null || true

# spimdisasm must stay below 2.0: splat 0.37.1 pins >=1.39.0,<2.0.0.
"$VENV/bin/pip" install -q \
    "PyYAML==6.0.3" \
    "pylibyaml==0.1.0" \
    "tqdm==4.67.1" \
    "intervaltree==3.1.0" \
    "colorama==0.4.6" \
    "spimdisasm>=1.39.0,<2.0.0" \
    "rabbitizer>=1.10.0,<2.0.0" \
    "pygfxd>=1.0.5" \
    "n64img>=0.1.4" \
    "crunch64>=0.2.0"

echo "--- installed ---"
"$VENV/bin/pip" list 2>/dev/null | grep -iE 'spim|rabbit|yaml|crunch|n64img|pygfxd|tqdm|interval' || true

echo "--- vendored splat responds ---"
cd "$SPLAT"
"$VENV/bin/python" split.py --help 2>&1 | head -12
