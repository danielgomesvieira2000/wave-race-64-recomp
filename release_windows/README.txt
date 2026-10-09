Wave Race 64 - Native C++ Port
================================

Release: 1.0.2 (Native C++ Standalone)
Build: 2026-10-09

WHAT THIS IS
------------
A pure C++ reimplementation of Wave Race 64 that runs without any ROM file
or recompiled MIPS code. Built with statically-linked SDL2.

REQUIREMENTS
------------
- Windows 10 or later (x64)
- NO install required - extract and run
- Includes MinGW runtime DLLs (portable)

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
