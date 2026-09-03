#!/usr/bin/env bash
# Phase 02: build the N64Recomp CLI under Linux; phase 05 added RSPRecomp.
#
# The Windows build of the tool dies with 0xC0000409 partway through writing its
# output, leaving funcs.h and the last funcs_*.c truncated mid-line. That status
# is a __fastfail, usually a detected stack overflow, and Windows defaults to a
# 1 MB stack where Linux gives 8 MB -- so the same recursion that overflows
# there is expected to complete here.
#
# The tool is built out of tree so the Windows build directory is left intact.
set -euo pipefail

REPO="/mnt/c/Users/Daniel/claude-projects/n64recomp_waverace64"
SRC="$REPO/lib/N64ModernRuntime/N64Recomp"
BUILD="$SRC/build-linux"

cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "$BUILD" --target N64RecompCLI 2>&1 | tail -4

# RSPRecomp turns the cartridge's audio microcode into C++. It is built from the
# same tree and is subject to the same patch discipline -- see
# tools/patch_rsprecomp.py, whose fix only reaches the port through this build.
cmake --build "$BUILD" --target RSPRecomp 2>&1 | tail -4

echo
ls -la "$BUILD/N64Recomp" "$BUILD/RSPRecomp"
