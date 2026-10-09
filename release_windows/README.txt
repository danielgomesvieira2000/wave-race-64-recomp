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
