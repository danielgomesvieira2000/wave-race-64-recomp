#!/usr/bin/env bash
# Build the Linux port. On the first build, pass your USA Rev A dump.
#
#   bash tools/setup_linux.sh
#   bash tools/build_linux.sh "/path/to/Wave Race 64 (USA) (Rev A).z64"
#   ./build-linux/WaveRace64Recomp
#
# Afterwards, source-only rebuilds need no argument: the generated sources are
# already there and only change when the dump does.
#
# Environment:
#   WR64_BUILD_DIR   where to build (default build-linux)
#   WR64_JOBS        parallelism (default: the number of processors)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

if [ "$(uname -s)" != Linux ]; then
    echo "This script is for Linux. On macOS use tools/build_macos.sh." >&2
    exit 1
fi

BUILD_DIR="${WR64_BUILD_DIR:-build-linux}"
JOBS="${WR64_JOBS:-$(nproc)}"

for tool in clang clang++ cmake ninja python3; do
    command -v "$tool" > /dev/null || {
        echo "$tool is not on PATH. Run: bash tools/setup_linux.sh" >&2
        exit 1
    }
done

# --------------------------------------------------------------- patches ----
# Every one of these is idempotent and every one is required: the port links
# against symbols they add. They patch submodules, so a submodule update
# reverts them silently and they are re-applied on every build rather than
# once. patch_rt64.py runs the rest of the RT64 ones itself.
echo "=== patches ==="
for patch in patch_rt64.py patch_n64recomp.py patch_rsprecomp.py \
             patch_librecomp.py patch_recompinput.py; do
    python3 "tools/$patch"
done

# ------------------------------------------------------- generated sources ----
if [ $# -gt 0 ]; then
    echo
    echo "=== recompiler ==="
    bash tools/wsl_build_recompiler.sh
    echo
    python3 tools/generate_game.py "$1"
elif [ ! -f RecompiledFuncs/funcs.h ]; then
    echo "First build: pass your dump." >&2
    echo "    bash tools/build_linux.sh \"/path/to/Wave Race 64 (USA) (Rev A).z64\"" >&2
    exit 1
fi

# ------------------------------------------------------------ configure ----
# CMAKE_POLICY_VERSION_MINIMUM is for CMake 4, which hard-errors on the
# cmake_minimum_required(VERSION <3.5) still declared by six example and CI
# files under lib/RT64/src/contrib. See docs/BUILDING.md, "Known issues".
echo
echo "=== configure ==="
cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DWR64_WITH_RUNTIME=ON -DWR64_WITH_RECOMPILED=ON -DWR64_WITH_FRONTEND=ON

echo
echo "=== build ==="
cmake --build "$BUILD_DIR" --target WaveRace64Recomp -j "$JOBS"

echo
echo "Built: $BUILD_DIR/WaveRace64Recomp"
echo "Settings and saves live in \${XDG_DATA_HOME:-~/.local/share}/WaveRace64Recomp."
