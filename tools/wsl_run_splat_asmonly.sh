#!/usr/bin/env bash
# Phase 01: produce an assembly-only disassembly of the Rev A dump.
#
# Our config lives in recomp/ and is committed; splat runs inside the reference
# decomp checkout because the config's symbol tables, linker scripts and splat
# extensions are all relative to that tree. So the config is copied in at run
# time rather than the tree being restructured around it.
set -euo pipefail

VENV="$HOME/wr64venv"
REPO="/mnt/c/Users/Daniel/claude-projects/n64recomp_waverace64"
DECOMP="$REPO/reference/wr64-decomp"
CONFIG="wr64.us.rev1.asm.yaml"

cd "$DECOMP"

if [ ! -f baserom.us.rev1.z64 ]; then
    echo "baserom.us.rev1.z64 missing from $DECOMP" >&2
    exit 1
fi

cp "$REPO/recomp/$CONFIG" "./$CONFIG"
mkdir -p linker_scripts/us/rev1/auto-asmonly

echo "=== splat (asm-only) ==="
"$VENV/bin/python" tools/splat/split.py "$CONFIG" --disassemble-all "$@" 2>&1 \
    | grep -vE 'it/s\]|s/it\]' || true

echo
echo "=== output ==="
echo "asm-all .s files      : $(find asm-all -name '*.s' 2>/dev/null | wc -l)"
echo "function labels       : $(grep -rho '^glabel [A-Za-z_][A-Za-z0-9_]*' asm-all 2>/dev/null | wc -l)"
echo "linker script         : $(wc -l < linker_scripts/us/rev1/waverace64.asmonly.ld 2>/dev/null || echo missing) lines"

echo
echo "=== does the linker script still reference C objects? ==="
c_objs=$(grep -oE 'build/src/[^ ]*\.o' linker_scripts/us/rev1/waverace64.asmonly.ld 2>/dev/null | wc -l)
asm_objs=$(grep -oE 'build/asm-all/[^ ]*\.o' linker_scripts/us/rev1/waverace64.asmonly.ld 2>/dev/null | wc -l)
echo "build/src/*.o      : $c_objs   (must be 0 for an asm-only link)"
echo "build/asm-all/*.o  : $asm_objs"
