#!/usr/bin/env bash
# Build the macOS app. On the first build, pass your USA Rev A dump.
#
#   bash tools/setup_macos.sh
#   bash tools/build_macos.sh "/path/to/Wave Race 64 (USA) (Rev A).z64"
#   open build-macos/WaveRace64Recomp.app
#
# Afterwards, source-only rebuilds need no argument: the generated sources are
# already there and only change when the dump does.
#
# Environment:
#   WR64_BUILD_DIR           where to build (default build-macos)
#   WR64_JOBS                parallelism (default: the number of cores)
#   WR64_MACOS_TARGET        deployment target (default 15.0)
#   WR64_DEPENDENCY_PREFIX   use separately built SDL2/FreeType/libpng from
#                            here instead of Homebrew's, for a redistributable
#                            build -- see tools/build_macos_dependencies.sh
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

if [ "$(uname -s)" != Darwin ]; then
    echo "This script is for macOS. On Linux use tools/build_linux.sh." >&2
    exit 1
fi

BUILD_DIR="${WR64_BUILD_DIR:-build-macos}"
JOBS="${WR64_JOBS:-$(sysctl -n hw.ncpu)}"

# RT64 compiles its shaders to Metal with the toolchain inside Xcode, and the
# command-line tools alone are not enough. Point DEVELOPER_DIR at an Xcode if
# one has not been selected already.
if [ -z "${DEVELOPER_DIR:-}" ]; then
    for candidate in /Applications/Xcode.app/Contents/Developer \
                     /Applications/Xcode-beta.app/Contents/Developer; do
        if [ -d "$candidate" ]; then export DEVELOPER_DIR="$candidate"; break; fi
    done
fi

# binutils built by setup_macos.sh, and the splat virtualenv, ahead of the path.
export PATH="$REPO/build-toolchain/install/bin:${WR64_VENV:-$REPO/.venv}/bin:$PATH"

CMAKE_ARGS=(-DCMAKE_OSX_DEPLOYMENT_TARGET="${WR64_MACOS_TARGET:-15.0}")
if [ -n "${WR64_DEPENDENCY_PREFIX:-}" ]; then
    # Homebrew bottles are built for the running OS and may require a newer one
    # than the bundle targets, so a redistributable build takes its libraries
    # from somewhere they were built against the same target.
    CMAKE_ARGS+=(
        "-DCMAKE_PREFIX_PATH=$WR64_DEPENDENCY_PREFIX"
        "-DSDL2_DIR=$WR64_DEPENDENCY_PREFIX/lib/cmake/SDL2"
        "-DFREETYPE_INCLUDE_DIR_freetype2=$WR64_DEPENDENCY_PREFIX/include/freetype2"
        "-DFREETYPE_INCLUDE_DIR_ft2build=$WR64_DEPENDENCY_PREFIX/include/freetype2"
        "-DFREETYPE_LIBRARY_RELEASE=$WR64_DEPENDENCY_PREFIX/lib/libfreetype.dylib"
    )
fi

# --------------------------------------------------------------- patches ----
# Every one of these is idempotent and every one is required: the port links
# against symbols they add. They patch submodules, so a submodule update
# reverts them silently and they are re-applied on every build rather than
# once. patch_rt64.py runs the rest of the RT64 ones itself, including
# patch_macos.py.
echo "=== patches ==="
for patch in patch_rt64.py patch_n64recomp.py patch_rsprecomp.py \
             patch_librecomp.py patch_recompinput.py; do
    python3 "tools/$patch"
done

# ------------------------------------------------------ generated sources ----
if [ $# -gt 0 ]; then
    echo
    echo "=== recompiler ==="
    bash tools/wsl_build_recompiler.sh
    echo
    python3 tools/generate_game.py "$1"
elif [ ! -f RecompiledFuncs/funcs.h ]; then
    echo "First build: pass your dump." >&2
    echo "    bash tools/build_macos.sh \"/path/to/Wave Race 64 (USA) (Rev A).z64\"" >&2
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
    "${CMAKE_ARGS[@]}" \
    -DWR64_WITH_RUNTIME=ON -DWR64_WITH_RECOMPILED=ON -DWR64_WITH_FRONTEND=ON

echo
echo "=== build ==="
cmake --build "$BUILD_DIR" --target WaveRace64Recomp -j "$JOBS"

echo
echo "=== bundle ==="
python3 tools/package_macos.py "$BUILD_DIR/WaveRace64Recomp.app"

echo
echo "Built: $BUILD_DIR/WaveRace64Recomp.app"
echo "Settings and saves live in ~/Library/Application Support/WaveRace64Recomp."
