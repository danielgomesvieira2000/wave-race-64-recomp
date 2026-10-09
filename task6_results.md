# Task 6: Custom Course Loader Prototype — Results

## Summary

Implemented a custom course loader prototype that parses Wave Race 64 course data from JSON format, providing a `CourseManager` API under `wr64::courses`. A proof-of-concept course (Sunset Harbor) demonstrates the full loader pipeline.

## What Was Built

### 1. Header: `include/wr64/courses.hpp`

Defines the `wr64::courses` namespace API as specified in `task3_abstraction_api.md`:

- **`CourseId`** enum matching the N64 ROM course list (Dolphin Park=0 through Rider Selection=9, plus Sunset Harbor=10 for the custom course)
- **`Difficulty`** enum (Normal/Hard/Expert)
- **`StartPosition`, `Buoy`, `Gate, `WaterParams`** — data structs mirroring the game's internal structures
- **`CourseParams`** — the full course definition (name, draw distance, buoys, gates, start positions, wave parameters, lap/power-miss counts)
- **`Course`** — a loaded course instance with factory `create()` and `load()` / `load_from_file()` methods
- **`CourseLoader`** — abstract loader interface for format-specific loaders
- **`JsonCourseLoader`** — nlohmann/json-based JSON loader implementing `CourseLoader`
- **`CourseManager`** — singleton managing registered courses, current course, progress tracking, and dynamic wave-level / lap configuration

### 2. Implementation: `src/courses/course_loader.cpp`

- **Builtin course table**: mirrors the N64 ROM's hardcoded data tables:
  - `D_800DAB10` → draw distances (5000 for courses 0-1, 6000 for course 2)
  - `D_800EAAEC` → wave level tables (from reference decompilation)
  - `D_800DA988` → `{selector, power_misses, lap_count}` per difficulty
- **JSON loader** (`JsonCourseLoader::load`): parses all `CourseParams` fields including bounds, file references, start positions, buoys, gates, wave params per difficulty, laps, and power misses
- **CourseManager**: singleton with `register_custom_course`, `get_course`, `set_current_course`, progress tracking (`save_course_completion`, `get_best_time`), and dynamic configuration (`set_wave_level`, `set_laps`)

### 3. Sample Course: `assets/courses/sunset_harbor.json`

Proof-of-concept course (id=10), "Sunset Harbor", built on Sunset Bay (course id 2) geometry with:
- 4 start positions for multiplayer
- 12 buoys (mixed small/racing types)
- 3 gate markers
- Wave parameters: Normal=1.0, Hard=1.5, Expert=2.0
- 3 laps, 5 power misses across all difficulties

### 4. Vendored Dependency: `third_party/nlohmann/json.hpp`

Single-header JSON library (v3.11.3) added for JSON parsing. The `CMakeLists.txt` already included `third_party` in `target_include_directories`, so `#include <nlohmann/json.hpp>` resolves correctly.

### 5. Build Integration: `CMakeLists.txt`

- Added `src/courses/course_loader.cpp` to `WR64_SOURCES`
- Added `third_party/` to include directories

### 6. Test Program: `src/courses/verify_course_loader.cpp`

Standalone verification with 9 test cases covering:
1. Loading Sunset Harbor from JSON (all fields verified)
2. Start position parsing (x, z, heading)
3. Wave parameters per difficulty
4. Race settings (laps, power misses)
5. CourseManager registration and retrieval
6. Wave level configuration API
7. Progress tracking (best time updates)
8. Builtin course loading (Sunset Bay)
9. Error handling (nonexistent file returns nullptr)

## Key Data Sources

| Source | Used For |
|--------|----------|
| `reference/wr64-decomp/include/course.h` | Course enum values |
| `reference/wr64-decomp/src/game/code_4C750.c` | D_800EABEC (wave levels), D_800DA988 (laps/power misses), D_800DAB10 (draw distances) |
| `docs/GAME-INTERNALS.md` | Draw distance field at `+0xA4` (5000/6000), buoy slot limit (32), func_80071E70 (buoy list builder), func_8006E674 (buoy culling) |
| `assets/water/profiles.json` | Water profile format (used as reference for per-course water params) |
| `task3_abstraction_api.md` | CourseManager API design spec |

## Verification

All 9 tests pass:

```
=== Test 1: Load Sunset Harbor from JSON ===
  name:            Sunset Harbor
  id:              10
  draw_distance:   6000
  start_positions: 4 entries
  buoys:           12 entries
  gates:           3 entries
  PASSED

=== Test 5: CourseManager registration ===
  Registered and retrieved: Sunset Harbor
  Current course: 10
  Available courses: 10 (including builtins)
  PASSED

=== Test 9: Nonexistent file ===
  Correctly returned nullptr for nonexistent file
  PASSED

=== ALL TESTS PASSED ===
```

## Files Created/Modified

| File | Status |
|------|--------|
| `include/wr64/courses.hpp` | Created |
| `src/courses/course_loader.cpp` | Created |
| `src/courses/verify_course_loader.cpp` | Created |
| `assets/courses/sunset_harbor.json` | Created |
| `third_party/nlohmann/json.hpp` | Created (vendored v3.11.3) |
| `CMakeLists.txt` | Modified (added course_loader.cpp source, third_party include dir) |

## Notes

- The loader uses C++20 as required, with nlohmann/json for JSON parsing
- Builtin courses (0-8) use ROM-derived data tables; custom courses load from JSON
- The `Course::create()` factory pattern avoids exposing the private constructor through `std::make_unique`
- The Sunset Harbor course is on a "custom course geometry based on Sunset Bay" — start positions, buoys, and gates are defined as proof-of-concept race data, while water profiles reference the existing `assets/water/profiles.json`
- The test binary (`verify_loader`) compiles standalone with `clang++ -std=c++20 -Iinclude -Ithird_party`