# Documentation

## Start here

| | |
|---|---|
| [BUILDING.md](BUILDING.md) | From an empty Windows, Linux or macOS machine to the first race: the toolchain, the dump, splat, N64Recomp, the submodule patches, CMake. |
| [HUD-INSPECTOR.md](HUD-INSPECTOR.md) | The debug menu on **F1** — how to use it to find and fix something drawn in the wrong place, and a recipe for putting the same tool in another N64 port. |
| [WATER.md](WATER.md) | The modern water renderer: what each setting does and what it costs, and a recipe for a shading replacement that leaves a game's own simulation alone. Original, a true bypass, by default. |
| [RENDER-DISTANCE-CENSUS.md](RENDER-DISTANCE-CENSUS.md) | Measuring which objects a game stops drawing at a distance and which it does not — how to run the census here, and a recipe for the same measurement in another N64 port. |
| [TEXTURE-PACK-MATCHING.md](TEXTURE-PACK-MATCHING.md) | Turning a texture pack made for another emulator (Dolphin, the Wii Virtual Console release) into a pack this port loads, by matching pictures against an RT64 dump -- how to run it, and how the dump decoding and matching work, for another port. |
| [../examples/mods/README.md](../examples/mods/README.md) | Making a mod: the `.nrm` format, `mod.json`, and how to build a texture pack. |
| [../CONTRIBUTING.md](../CONTRIBUTING.md) | The rules, chiefly about keeping the game's data out of this repository. |
| [../CHANGELOG.md](../CHANGELOG.md) | What changed in each release. |

## The technical reference

Documents written for anyone working on the Wave Race 64 decompilation or on
another N64 port. They are kept current as the port changes — a change that
discovers a fact about the game or fixes something in the toolchain updates one
of them in the same commit.

| | |
|---|---|
| [GAME-INTERNALS.md](GAME-INTERNALS.md) | **The game.** Cartridge identity, code and overlay layout, the main loop, the game-state machine, the overlay table, how a frame's display list is built, the 303×199 drawn region, the world's frustum, how a 3D object is placed inside a 2D layout, the sky and water geometry, reading a race out of RDRAM, and the audio microcode. |
| [WATER-IMPLEMENTATION.md](WATER-IMPLEMENTATION.md) | **The water renderer, in full.** What the fork built -- the game-side boundary, the material ABI, the extended GBI transport, the RT64 passes, the shaders, the measured cost on Apple silicon -- and what this repository took, changed, dropped and added, including the two bugs that only appeared here. |
| [TRANSFORM-PAIRING.md](TRANSFORM-PAIRING.md) | **Interpolation that pairs the right cameras, riders and objects.** How RT64 pairs each frame's cameras and objects with the last frame's; the two-player race-start burst (split-screen views paired with each other's cameras), riders coming apart (parts paired with their mirrored twins), and objects paired across the course, each measured and captured; the fixes; and the tools that found them -- a pairing log, a window capture that ignores what covers the game, and input scripts for a 2P race and for switching riders. A manual and a recipe for another port. |
| [PORTING.md](PORTING.md) | **The toolchain and runtime.** Getting a byte-exact ELF out of splat, N64Recomp's input modes and failure messages, overlay dispatch, the librecomp/ultramodern harness, recompiling RSP audio microcode, the RT64 patches and the display-list rewriter behind widescreen and interpolation, mods, the diagnostics worth building, and the measurement traps that cost the most time. Written symptom-first. |

## The working record

| | |
|---|---|
| [PLAN.md](PLAN.md) | The build plan the project was executed against, phase by phase, with its standing constraints. A historical document — see the note at its head for where it was overtaken. |
| [findings/](findings) | What each phase actually found, including the wrong turns. The reference above is the distilled version; these are where it came from. |
| [releases/](releases) | The full notes for every release, with the reasoning and the measurements behind each change. |
