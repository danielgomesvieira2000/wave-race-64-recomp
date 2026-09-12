#!/usr/bin/env bash
# Phase 01: disassemble the Rev A dump with the config written for it.
#
# We run splat inside the reference decomp checkout rather than copying its
# config into our tree first. The config references relative paths for symbol
# tables, linker scripts and splat extensions, and this is the environment it
# was written to run in -- proving it works unmodified is the point of this
# step. Our own config gets derived from it once we know what it produces.
set -euo pipefail

# Derived rather than hardcoded: these scripts also run natively on Linux and
# macOS, where the checkout is not under /mnt/c.
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# The in-tree .venv the setup scripts make, unless one is named explicitly.
# $HOME/wr64venv is still honoured for checkouts set up before this.
VENV="${WR64_VENV:-$REPO/.venv}"
if [ ! -d "$VENV" ] && [ -d "$HOME/wr64venv" ]; then VENV="$HOME/wr64venv"; fi
DECOMP="$REPO/reference/wr64-decomp"

cd "$DECOMP"

if [ ! -f baserom.us.rev1.z64 ]; then
    echo "baserom.us.rev1.z64 missing from $DECOMP" >&2
    exit 1
fi

echo "=== sha1 of the input ==="
sha1sum baserom.us.rev1.z64

echo
echo "=== splat ==="
"$VENV/bin/python" tools/splat/split.py waverace64.us.rev1.yaml "$@"

echo
echo "=== what came out ==="
for d in asm bin linker_scripts/us/rev1/auto; do
    if [ -d "$d" ]; then
        printf '%-34s %s files\n' "$d" "$(find "$d" -type f | wc -l)"
    fi
done
echo
echo "asm .s files: $(find asm -name '*.s' 2>/dev/null | wc -l)"
echo "glabel count (function labels): $(grep -rho '^glabel [A-Za-z_][A-Za-z0-9_]*' asm 2>/dev/null | wc -l)"
