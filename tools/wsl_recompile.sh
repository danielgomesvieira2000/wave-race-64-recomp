#!/usr/bin/env bash
# Phase 02: run the recompiler and regenerate the declarations header.
#
# The recompiler runs under Linux deliberately. The Windows build of the same
# tool dies with 0xC0000409 partway through writing its output, leaving funcs.h
# and the last funcs_*.c truncated mid-line -- and truncated output still looks
# like success unless you compile it. Linux's larger default stack completes.
set -euo pipefail

REPO="/mnt/c/Users/Daniel/claude-projects/n64recomp_waverace64"
TOOL="$REPO/lib/N64ModernRuntime/N64Recomp/build-linux/N64Recomp"

if [ ! -x "$TOOL" ]; then
    echo "recompiler not built -- run tools/wsl_build_recompiler.sh first" >&2
    exit 1
fi

cd "$REPO"
mkdir -p RecompiledFuncs

echo "=== recompile ==="
"$TOOL" recomp/wr64.toml 2> /tmp/wr64_recomp_err.txt || true
grep -v '^\[WARN\]' /tmp/wr64_recomp_err.txt | tail -5 || true

echo
echo "=== declarations for runtime-provided libultra ==="
python3 tools/gen_reimplemented_decls.py

echo
echo "=== output ==="
echo "files        : $(ls RecompiledFuncs | wc -l)"
echo "declarations : $(grep -c ';' RecompiledFuncs/funcs.h)"
# Truncated output is the failure mode that matters, so check the end markers.
tail -1 RecompiledFuncs/funcs.h | grep -q '#endif' \
    && echo "funcs.h      : complete" \
    || { echo "funcs.h      : TRUNCATED -- the recompiler died mid-write" >&2; exit 1; }
