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

# Copy SDL2 DLL (required at runtime)
if [ -f "/tmp/SDL2-2.30.8/x86_64-w64-mingw32/bin/SDL2.dll" ]; then
    cp "/tmp/SDL2-2.30.8/x86_64-w64-mingw32/bin/SDL2.dll" "$OUT_DIR/"
    echo "Copied: SDL2.dll"
else
    echo "WARNING: SDL2.dll not found - users need separate SDL2 runtime"
fi

# Copy course files
mkdir -p "$OUT_DIR/assets/courses"
if [ -d "assets/courses" ]; then
    cp assets/courses/*.json "$OUT_DIR/assets/courses/" 2>/dev/null || true
    echo "Copied course files"
fi

# Copy texture assets (optional - for full experience)
# Note: 4,032 textures are extracted in assets/task4_extracted/
# Include a sample subset for testing
if [ -d "assets/task4_extracted" ]; then
    mkdir -p "$OUT_DIR/assets/textures/sample"
    # Copy first 100 textures as example
    cp assets/task4_extracted/*.png 2>/dev/null | head -100 | xargs -I{} cp {} "$OUT_DIR/assets/textures/sample/" 2>/dev/null || true
    echo "Copied sample textures (100 files)"
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
or recompiled MIPS code. Built with SDL2 for rendering.

REQUIREMENTS
------------
- Windows 10 or later (x64)
- Visual C++ Redistributable 2015+ (or DLLs bundled above)

TO RUN
------
To run the game, extract this folder and double-click WaveRace64Recomp.exe.

If SDL2.dll is missing, download it from:
https://www.libsdl.org/download-2.0.php

FEATURES
--------
- Native water physics (func_8004D30C reimplemented in C++)
- JSON-based course loader
- LAN multiplayer via ENet (enables with CMake)
- Pure C++ - no ROM needed

CONTROLS
--------
- Arrow keys / WASD: Move
- Space: Accelerate / Jump
- ESC: Exit

BUILD INSTRUCTIONS
------------------
For developers:

cmake -B build_windows -G Ninja \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix \
    -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix \
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
echo "Output: $OUT_DIR/WaveRace64_${VERSION}_windows.zip"
ls -la "WaveRace64_${VERSION}_windows.zip"