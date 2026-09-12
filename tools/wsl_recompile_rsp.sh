#!/usr/bin/env bash
# Phase 05: recompile the audio microcode.
#
# Separate from tools/wsl_recompile.sh because it is a different tool with a
# different config: N64Recomp turns the game's MIPS into C, RSPRecomp turns the
# RSP's microcode into C++ with vector intrinsics. Both run under Linux for the
# same reason -- see the note at the top of wsl_recompile.sh.
#
# This regenerates a file that is not committed (RecompiledFuncs/ is derived
# from the cartridge), so it has to be rerunnable from nothing.
set -euo pipefail

# Derived rather than hardcoded: these scripts also run natively on Linux and
# macOS, where the checkout is not under /mnt/c.
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOL="$REPO/lib/N64ModernRuntime/N64Recomp/build-linux/RSPRecomp"

if [ ! -x "$TOOL" ]; then
    echo "RSPRecomp not built -- run tools/wsl_build_recompiler.sh first" >&2
    exit 1
fi

cd "$REPO"
mkdir -p RecompiledFuncs

echo "=== recompile aspMain ==="
"$TOOL" recomp/aspMain.rsp.toml

OUT="RecompiledFuncs/aspMain_rsp.cpp"
echo
echo "=== output ==="
echo "lines   : $(wc -l < "$OUT")"
echo "targets : $(grep -c 'case 0x' "$OUT") entries in the indirect jump switch"
