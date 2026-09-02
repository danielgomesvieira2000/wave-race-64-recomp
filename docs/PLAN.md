# Build plan

## The decision this project is built on

N64Recomp needs symbols and section metadata, not just a ROM. Its config
(`src/config.cpp` upstream) accepts exactly one of two input modes:

- `elf_path` — **elf input mode**
- `symbols_file_path` + `rom_file_path` — **symbols file mode**

Symbols file mode is faster to a first result and needs no MIPS toolchain. It
is also a dead end for this project: upstream rejects `func_reference_syms_file`
and `data_reference_syms_files` outside elf input mode, which means the
reference-symbol workflow and the single-file-output patch workflow — the thing
that makes phase 04 iteration take seconds instead of minutes — are unavailable
for the life of the project.

**So: we assemble our own ELF.** Not a decompilation. An *assembly-only* ELF
built from splat output, which needs symbol names, addresses and sizes but no
recovered C and no matching build. Sources for those symbols, in order of trust:

1. [LLONSIT/Wave-Race-64](https://github.com/LLONSIT/Wave-Race-64) — WIP decomp
   of this exact revision. Its splat config is the starting point.
2. A JAL-target scan over `.text` for call targets splat did not classify as
   functions. WACOMalt's equivalent pass took 756 known functions to 1,228.
3. Byte-matching libultra functions against a known SDK build, so they can be
   renamed and handed to the runtime via `reimplemented_funcs` rather than
   recompiled.

Disagreements between those three sources are bugs, not noise. Wrong function
boundaries produce C that compiles and then corrupts state at runtime.

## Phases

Each gate is the entry condition for the next phase.

### 00 — Ground rules and skeleton
Repo, submodules, `.gitignore` that refuses game data, phase-gated CMake,
toolchain, ROM identification tool.
**Gate:** the tree configures and builds; `--identify` accepts your dump.

### 01 — Split the ROM
Port and verify the splat config. Enumerate overlay segments and their load
addresses. Run the JAL scan. Rename libultra functions.
**Gate:** an assembled ELF whose `.text` is byte-identical to the ROM's code.

### 02 — First recompile
Write `recomp/wr64.toml`, run N64Recomp, compile the output. Script the fixes
for writes to `$zero` and unresolved jump tables; never hand-edit generated C.
Keep `--dump-context` output in tree as the symbol reference patches link against.
**Gate:** all `funcs_*.c` compile and link into a static library.

### 03 — Runtime harness
Register RT64. Wire VI timing. Controller callback into `OSContPad` with the
analog curve right — Wave Race is unusually sensitive to stick response. Audio
callback. EEPROM save through librecomp, to a per-user path. ROM ingest on
first run.
**Gate:** the executable reaches `recomp_entrypoint` and runs the first thread.

### 04 — Boot bring-up
The long phase, and where both public Wave Race 64 attempts are parked.
Overlay relocations first: if the ROM's overlay tables do not carry relocation
data the tooling recognizes, reverse the loader's format and drive
`recomp_load_overlays` / `overlay_apply_relocations` from a `RECOMP_PATCH`.
Expect to land on librecomp's `osEPiRawStartDma` guard repeatedly. Instrument
with a function-entry trace; a debugger on 19 MB of generated C is not a plan.
**Gate:** logo, then the attract-mode demo, rendering recognizably.

### 05 — Graphics and audio correctness
Confirm the F3DEX variant and that RT64 dispatches it. Verify the water
surface — the game's signature effect, with a documented history of breaking
under HLE renderers. Audio through ultramodern. Play every course in both
directions plus stunt mode.
**Gate:** a full championship completes with correct visuals, audio and records.

### 06 — Enhancements and release
Widescreen and arbitrary resolution through RT64, RecompFrontend for menus and
rebindable input. High frame rate is invasive here — physics and camera are
tied to a 30 Hz update — so patch deliberately rather than hope. Then CI that
builds without a ROM, and a first-run flow that explains the ROM requirement.
**Gate:** a stranger with a dump and no context can build and play it.

## Standing constraints

- No ROM, asset, or ROM-derived file is ever committed. The user supplies the
  dump at build time.
- Generated code is never hand-edited. If the output is wrong, fix the config
  or write a script in `tools/`.
- Every phase is entered only through the previous phase's gate.
