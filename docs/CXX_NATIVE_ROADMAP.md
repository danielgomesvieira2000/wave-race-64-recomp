# C++ Native Rewrite Roadmap — Independent Port

## Current State

**Phase 0 (Complete):** Tasks 1-4 — Codebase inventory, dependency mapping, API design, asset extraction.
**Phase 1 (Complete):** Tasks 5-8 — Three C++ native implementations proven:
- `func_8004D30C` water height query (41,215 tests pass, 772ns/query)
- Course loader (9/9 tests pass, JSON-based)
- LAN multiplayer via ENet (bidirectional sync verified)
- Performance benchmark (1.29M queries/sec)

**Phase 2 (Current):** Integration planning — make the port build and run independently of recompiled MIPS code.

## Architecture: Hybrid Approach

The current port uses **N64Recomp** to generate C from MIPS assembly, then links with the native port (`src/`). The hybrid approach keeps the recompiled code for game logic that hasn't been reimplemented yet, and gradually replaces functions with native C++ as they are reimplemented.

### Key Integration Patterns

1. **Patches/Wrappers (`patches/`)** — Hook functions that wrap recompiled calls:
   - `patches/water.cpp` — Wraps `func_8009345C` (race init) and `func_80046CF8` (gfx task submit)
   - `patches/buoys.cpp` — Hooks `func_8006E674` (buoy rendering) with address-based overrides
   - `patches/framerate.cpp` — Frame rate reporting at `osViSwapBuffer`
   - `patches/dma.cpp` — Synchronous ROM DMA hook

2. **Native reimplementation (`src/`)** — Complete C++ replacements:
   - `src/engine/gamemain.cpp` — Native `func_8004D30C` (water height)
   - `src/courses/course_loader.cpp` — Native course management
   - `src/network/lan_session.cpp` — Native ENet multiplayer

3. **RDRAM bridge** — Native code reads/writes game state in RDRAM via helpers in `src/haptics.cpp` and the `read_word`/`read_float` functions found in `patches/buoys.cpp`

## Implementation Strategy

### Phase 2: Native Build Without Recompiled Code

**Goal:** Build the port without `WR64_WITH_RECOMPILED=ON` — no generated C code, pure native C++.

**Steps:**

1. **Native entry point** — Create `src/native_main.cpp` that initializes the game state without the recompiled code, using the native water engine, course loader, and placeholder game loop.

2. **ROM-independent asset loading** — Replace RDRAM-based asset access with direct file loading:
   - Use extracted assets from `assets/task4_extracted/` (4,032 PNG textures)
   - Use `assets/courses/sunset_harbor.json` for course geometry
   - Implement model/texture loaders for the extracted format

3. **Native game loop** — Replace the recompiled `Main_Thread` with a C++ main loop:
   - Input handling via SDL2
   - Game state updates (physics, AI) in C++
   - Rendering via RT64
   - Audio via SDL2

### Phase 3: Progressive Function Replacement

Replace recompiled functions one-by-one with native C++ equivalents, starting with the most isolated:

| Function | Address | Description | Complexity |
|----------|---------|-------------|------------|
| `func_8004D30C` | 0x8004D30C | Water height query | ✅ DONE |
| `func_8006E674` | 0x8006E674 | Buoy rendering | Medium |
| `func_80092CF0` | 0x80092CF0 | Game renderer dispatch | High |
| `func_800922E4` | 0x800922E4 | Controller update | Low |
| `func_8009345C` | 0x8009345C | Race initialization | Medium |
| `func_8004F3D4` | 0x8004F3D4 | Wave field generator | Medium |

### Phase 4: Windows Release Build

**Goal:** Produce a Windows executable that runs without any ROM file.

**Steps:**

1. **Windows toolchain** — Use MSVC or Clang-cl with Ninja
2. **Cross-compilation** — Build on Linux for Windows target (or use WSL with MSVC)
3. **Asset bundling** — Package textures, courses, audio into the executable
4. **Installer** — Create a distributable installer with all dependencies

## Files Requiring Changes

### New Files:
- `src/native_main.cpp` — Native entry point (no ROM, no recompiled code)
- `include/wr64/engine/game_state.hpp` — C++ game state manager
- `include/wr64/engine/renderer.hpp` — Native renderer wrapper
- `include/wr64/engine/audio.hpp` — SDL2 audio engine
- `assets/native/coursedata/` — Native course data directory
- `CMakeLists.txt` additions for native-only build targets

### Modified Files:
- `CMakeLists.txt` — Add `WR64_BUILD_NATIVE` option for standalone builds
- `src/callbacks.cpp` — Conditionally compile without recompiled code paths
- `src/renderer.cpp` — Use native water field instead of RDRAM dump
- `patches/*.cpp` — Guard behind `WR64_WITH_RECOMPILED`

## CMake Options

```
-D WR64_BUILD_NATIVE=ON        # Build standalone C++ port (no ROM, no recompiled code)
-D WR64_WITH_RUNTIME=OFF       # Don't link N64ModernRuntime
-D WR64_WITH_RECOMPILED=OFF    # Don't compile generated C code
-D WR64_WITH_NETWORK=ON        # Include ENet multiplayer
-D WR64_WITH_FRONTEND=ON       # Use RecompFrontend for menus
```

## Success Criteria

1. Game runs from Windows executable without any ROM file
2. All 41,215 water height tests pass on Windows
3. Course loader works with packaged JSON course data
4. LAN multiplayer session works between two Windows instances
5. Frame rate matches or exceeds the hybrid build
