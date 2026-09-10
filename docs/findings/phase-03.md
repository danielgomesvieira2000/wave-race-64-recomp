# Phase 03: runtime harness

**Gate met.** The runtime starts, RT64 initialises a real GPU device, the
recompiled entry point is reached, and the game's own threads run:

```
Device Name: Intel(R) Iris(R) Xe Graphics
Device Vendor: 0x8086
[wr64] runtime initialised; entering recomp_entrypoint
Initializing recomp heap at offset 0x01000000 with size 0x1F000000
[wr64] game thread 1 created
[wr64] game thread 2 created
[wr64] game thread 3 created
[wr64] game thread 4 created
```

That heap line comes from the recompiled cartridge code, not from us: the game
is running, allocating its heap and creating its OS threads.

The gate is reported rather than inferred. `GameEntry` takes `on_init_callback`
and `thread_create_callback`, and both are wired to print, so "it reached the
entry point" is an observation instead of a guess about a process that has not
crashed.

```
cmake -B build-rt -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl \
      -DWR64_WITH_RUNTIME=ON -DWR64_WITH_RECOMPILED=ON
cmake --build build-rt
./build-rt/WaveRace64Recomp.exe baserom.us.rev1.z64
```

## What was written

| File | Role |
|---|---|
| `src/renderer.cpp` | `RendererContext` subclass binding RT64 |
| `src/callbacks.cpp` | input, audio, RSP, events, errors, threads, gfx, on SDL2 |
| `src/overlays.cpp` | hands the generated section and overlay tables to librecomp |
| `src/libultra_stubs.cpp` | six libultra functions neither side implements |
| `src/main.cpp` | `GameEntry` registration, ROM validation, startup |

`--version` now reports **22 code sections and 19 overlay sections**, read from
the generated tables at runtime, which is independent evidence that the overlay
data survived the whole pipeline.

## Everything RT64 declares is directory-scoped

Four separate failures, one cause. RT64 sets its compile definitions, include
paths, link paths and DLL copies with `add_compile_definitions`,
`include_directories`, `link_directories` and `configure_file` -- all of which
apply to targets declared inside RT64's own `CMakeLists.txt` and none of which
reach a target declared outside it. The library *names* do propagate through
`target_link_libraries`, which is what makes the failures confusing: the link
asks for `SDL2.lib` while having no idea where it lives.

Each had to be repeated on our target:

| Missing | Symptom |
|---|---|
| `SDL2.lib` search path | `lld-link: could not open 'SDL2.lib'` |
| `HLSL_CPU`, `NOMINMAX`, ... | hlsl++ fails to parse, hundreds of errors |
| RT64 include paths | RT64 headers not found |
| `dxcompiler.dll`, `dxil.dll` | `0xC0000135` at startup, no message at all |

The DLL one is the nastiest: Windows reports only a status code, naming neither
the DLL nor the fact that one is missing.

## Use clang-cl, not clang++

RT64 chooses its warning flags from `CMAKE_CXX_SIMULATE_ID`, so a Clang
targeting the MSVC ABI is handed `/W4` on the assumption that such a Clang is
`clang-cl`. `clang++` reaches the same ABI through the GNU driver and rejects
`/W4` outright, failing every RT64 translation unit.

Changing compiler also invalidates the CMake cache and the `-D` options do not
survive it, so reconfiguring in place silently comes back with the runtime
switched off.

## start_game() comes before start(), not after

`recomp::start()` spawns the game thread, which spins in `wait_for_game_started`,
and then enters its own main loop, which does not return until the user quits.
Calling `start_game()` after it therefore never runs. The symptom is a process
that lives happily with a window open, prints nothing, and never reaches the
entry point -- with no error anywhere, because nothing has gone wrong as far as
the runtime is concerned. It is simply waiting for a start that will never come.

`start()` also creates the window itself when none is supplied, so doing it in
advance just initialises SDL twice.

## RT64 must be told the microcode before a display list

`processDisplayLists` asserts `hleGBI != nullptr` and does not select the
graphics binary interface itself. An emulator leaves the task in DMEM for RT64
to read; a recompilation has no real DMEM, so nothing identifies the microcode
and the first display list aborts on

```
Assertion failed: hleGBI != nullptr, rt64_interpreter.cpp, line 157
```

`send_dl` now calls `interpreter->loadUCodeGBI(task->t.ucode, task->t.ucode_data,
true)` first, passing the addresses straight from the OSTask.

## Six libultra functions belonged to nobody

N64Recomp's `ignored_funcs` list marks routines it will not translate because
the runtime provides them. librecomp implements most, but not
`osPfsIsPlug`, `osPfsInit`, `__osPfsSelectBank`, `__osContRamRead`,
`__osContRamWrite`, `__osGetCause` or `send_packet`. They surface as undefined
symbols at link time with nothing to say whose job they were.

`src/libultra_stubs.cpp` supplies them. The five Controller Pak routines report
`PFS_ERR_NOPACK`, which is what the game would see on hardware with an empty
controller slot rather than a placeholder -- librecomp answers its own share of
that API the same way. `send_packet` is libultra's kernel debug server, talking
to development hardware over a link no player has.

## Relocation types outside the enum

N64Recomp casts an ELF relocation type straight to its own `RelocType`, which
covers values 0 to 7. Our ELF also carries `R_MIPS_PC16`, which is 10, and
nothing checks the range: the name lookup indexes past an eight-entry table and
`recomp_overlays.inl` is emitted with 39 entries reading `.type =  },` which do
not compile.

`tools/fix_overlay_relocs.py` discards them, having first verified that all 39
are section-local. That makes them genuine no-ops: relocating a section moves a
PC-relative branch and its target together, leaving the distance unchanged. The
tool refuses to discard anything if a single one turns out to cross sections.

They exist because splat declares functions with `glabel`, making them global,
and GNU as emits a relocation for a branch to a global symbol even when it
resolves inside the same section.

## Deliberate placeholders

Both are marked in the source and neither blocks the gate:

- **Audio output.** Samples are discarded and the queue always reports itself
  drained, which keeps the game's audio thread running at the right cadence
  instead of stalling on a queue that never empties.
- **RSP microcode.** Graphics tasks go to the renderer; what reaches the RSP
  callback is the audio microcode, which needs recompiling in its own right.
  Returning `nullptr` is permitted but makes librecomp print and exit, so a stub
  reports the task finished instead, letting boot proceed in silence.

## Next

Phase 04, boot bring-up. The three stubbed overlay-dispatch functions from
phase 02 are the known blocker, and nothing that reaches an overlay will work
until they are reimplemented.
