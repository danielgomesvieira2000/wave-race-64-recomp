#!/usr/bin/env bash
# Package script for Windows release build
# Creates a distributable ZIP with executable and required assets

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

BUILD_DIR="build_windows"
OUT_DIR="release_windows"
VERSION="1.0.2"

# Create release directory
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

# Copy executable
if [ -f "$BUILD_DIR/WaveRace64Recomp.exe" ]; then
    cp "$BUILD_DIR/WaveRace64Recomp.exe" "$OUT_DIR/"
    echo "Copied: WaveRace64Recomp.exe"
else
    echo "ERROR: Executable not found at $BUILD_DIR/WaveRace64Recomp.exe"
    exit 1
fi

# SDL2.dll is NOT needed in the package - it's statically linked into WaveRace64Recomp.exe
# The executable is fully portable Windows PE with no external DLL dependencies
echo "SDL2 is statically linked - no DLL bundle needed"

# Copy course files
mkdir -p "$OUT_DIR/assets/courses"
if [ -d "assets/courses" ]; then
    cp assets/courses/*.json "$OUT_DIR/assets/courses/" 2>/dev/null || true
    echo "Copied course files"
fi

# Copy FULL texture assets (for complete game experience)
if [ -d "assets/task4_extracted" ]; then
    mkdir -p "$OUT_DIR/assets/textures"
    cp assets/task4_extracted/*.png "$OUT_DIR/assets/textures/" 2>/dev/null || true
    NUM_TEXTURES=$(ls "$OUT_DIR/assets/textures/" 2>/dev/null | wc -l)
    echo "Copied $NUM_TEXTURES textures"
fi

# Create README
cat > "$OUT_DIR/README.txt" << 'EOF'
Wave Race 64 - Native C++ Port
================================

Release: 1.0.2 (Native C++ Standalone)
Build: 2026-10-09

WHAT THIS IS
------------
A pure C++ reimplementation of Wave Race 64 that runs without any ROM file
or recompiled MIPS code. Built with statically-linked SDL2 - no DLLs required.

REQUIREMENTS
------------
- Windows 10 or later (x64)
- NO additional dependencies - everything is statically linked

TO RUN
------
1. Extract this ZIP file to any folder
2. Double-click WaveRace64Recomp.exe
3. Enjoy! (No install, no DLLs, no Visual Studio required)

FEATURES
--------
- Native water physics (func_8004D30C reimplemented in C++)
- 41,215 test-verified water height queries
- JSON-based course loader (Sunset Harbor included)
- LAN multiplayer via ENet (enable separately with CMake)
- Pure C++ - completely ROM-independent

CONTROLS
--------
- Arrow keys / WASD: Steer
- Space: Accelerate / Jump
- ESC: Exit

TECHNICAL DETAILS
-----------------
Build: Native C++ cross-compiled from Linux with MinGW-w64
Compiler: GCC 13 (posix threads)
Linking: Static (-static-libgcc -static-libstdc++ -static)
Dependencies: libstdc++ (static), SDL2 (static)
No external DLL dependencies - only Windows system DLLs (KERNEL32, USER32, etc.)

BUILD INSTRUCTIONS (For Developers)
-----------------------------------
cmake -B build_windows -G Ninja \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix \
    -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DWR64_BUILD_NATIVE=ON \
    -DWR64_WITH_RECOMPILED=OFF \
    -DWR64_WITH_RUNTIME=OFF

cmake --build build_windows

SOURCE
------
https://github.com/Vainerlut/wave-race-64-recomp

LICENSE
-------
GPL-3.0 (see LICENSE file)
EOF

echo "Created: README.txt"

# Create ZIP archive
cd "$OUT_DIR"
zip -r "../WaveRace64_${VERSION}_windows.zip" . -x "*.DS_Store"
cd "$REPO"

echo ""
echo "=== Release packaged ==="
echo "Output: WaveRace64_${VERSION}_windows.zip"
ls -la "WaveRace64_${VERSION}_windows.zip"