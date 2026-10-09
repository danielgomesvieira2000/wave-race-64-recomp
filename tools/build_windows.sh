#!/usr/bin/env bash
# Build script for Windows release build (cross-compilation from Linux)
#
# Prerequisites:
#   - x86_64-w64-mingw32-gcc or clang
#   - SDL2 development libraries for MinGW
#   - OR install vcpkg and create a toolchain file
#
# Usage:
#   bash tools/build_windows.sh
#
# Output:
#   build_windows/WaveRace64Recomp.exe
#

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

# Configuration
BUILD_DIR="build_windows"
WIN_TARGET="x86_64-w64-mingw32"

# Detect available cross-compiler
if command -v x86_64-w64-mingw32-clang++ &> /dev/null; then
    CC="x86_64-w64-mingw32-clang"
    CXX="x86_64-w64-mingw32-clang++"
    echo "Using clang cross-compiler"
elif command -v ${WIN_TARGET}-gcc &> /dev/null; then
    CC="${WIN_TARGET}-gcc"
    CXX="${WIN_TARGET}-g++"
    echo "Using GCC cross-compiler"
else
    echo "ERROR: No MinGW cross-compiler found."
    echo "Install with: sudo apt install mingw-w64"
    echo ""
    echo "Or use vcpkg toolchain:"
    echo "  ./vcpkg/bootstrap-vcpkg.sh"
    echo "  cmake -DCMAKE_TOOLCHAIN_FILE=../vcpkg/scripts/buildsystems/vcpkg.cmake ..."
    exit 1
fi

# Check for SDL2 MinGW libraries
SDL2_INCLUDE=""
SDL2_LIB=""

# Common locations for MinGW SDL2
for prefix in "/usr/${WIN_TARGET}" "/usr/${WIN_TARGET}/w64-mingw32" "/usr"; do
    if [ -d "${prefix}/include/SDL2" ]; then
        SDL2_INCLUDE="${prefix}/include"
    fi
    if [ -f "${prefix}/lib/${WIN_TARGET}/libSDL2.a" ] || [ -f "${prefix}/lib/libSDL2.dll.a" ]; then
        SDL2_LIB="${prefix}/lib/${WIN_TARGET}"
    fi
done

if [ -n "${SDL2_INCLUDE}" ] && [ -n "${SDL2_LIB}" ]; then
    echo "Found SDL2 at ${SDL2_INCLUDE} / ${SDL2_LIB}"
fi

# Build
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

cd "$BUILD_DIR"

cmake -GNinja \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DWR64_BUILD_NATIVE=ON \
    -DWR64_WITH_RECOMPILED=OFF \
    -DWR64_WITH_RUNTIME=OFF \
    -DWR64_WITH_NETWORK=OFF \
    -DWR64_WITH_FRONTEND=OFF \
    -DWR64_TESTS=OFF \
    ${SDL2_INCLUDE:+-DSDL2_INCLUDE_DIR="$SDL2_INCLUDE"} \
    ${SDL2_LIB:+-DCMAKE_LIBRARY_PATH="$SDL2_LIB"} \
    ..

ninja

echo ""
echo "=== Windows build complete ==="
echo "Executable: $BUILD_DIR/WaveRace64Recomp.exe"
echo ""
echo "To run on Windows, copy the .exe and the assets/ folder."
echo "Or use wine for testing:"
echo "  wine WaveRace64Recomp.exe"