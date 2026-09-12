# tools

Committed tooling. Everything here is original code that operates on a ROM the
user supplies; nothing here contains game data. The `wsl_*.sh` scripts run
under WSL *on Windows* because the disassembler and the MIPS assembler are Linux
tools; on Linux and macOS they run natively, and they derive the repository root
from their own location rather than assuming one. The rest run wherever Python
or PowerShell does.

`docs/BUILDING.md` says which to run and in what order. This is the index.

## Building the port

| Script | Purpose |
|---|---|
| `check_toolchain.ps1` | Reports which build tools are present on Windows. |
| `patch_n64recomp.py` | Exposes N64Recomp's `use_lookup_for_all_function_calls` as a config option, which overlay dispatch needs. Idempotent. |
| `patch_librecomp.py` | Makes librecomp's function-lookup failures report the address they failed on. Idempotent. |
| `patch_rsprecomp.py` | Makes RSPRecomp's indirect jumps ignore the low two bits of the target, as the hardware does. Idempotent. |
| `patch_recompinput.py` | Adds `players::auto_assign_controllers` to RecompFrontend, so the port can put the first pad on player one and the second on player two without the assignment modal. Idempotent. |
| `promote_hud_tags.py` | Copies the 2D tags you saved from the HUD inspector (`hud.json` in the per-user settings folder, outside the repository) into `load_defaults()` in `src/dlrewrite.cpp`, so they are compiled in and a release carries them. Rewrites one marker-delimited block, lower-cases identities because the lookup does, and is idempotent. `--dry-run` to look first, `--clear` to empty the local file afterwards. |
| `pack_mod.py` | Zips a mod directory into the `.nrm` the game installs, checking the required `mod.json` fields first and naming the file from the mod's id and version. `--install` writes it straight into the game's mods folder. |
| `patch_rt64_texturepacks.py` | Adds `RT64_SetTexturePacks` to RT64 so the port can hand it a list of texture packs to load; RT64 otherwise only accepts one through a file dialog in its developer UI. Applied on the next frame, because a mod can be enabled from another thread while the game runs. Idempotent, and chained into `patch_rt64.py`. |
| `patch_rt64_inspector.py` | Two patches about the debug menu. Adds one function pointer to RT64 (`RT64_PortInspectorHook`) that it calls once per frame from `State::inspect()` with an ImGui frame open, so the port can draw its own window inside RT64's developer UI -- null unless the port sets it -- and unbinds F2, whose session-wide ray tracing toggle is not something to leave under a finger once developer mode is on for everyone (F1, F3 and F4 stay). F1 opens it, in every build: the port turns RT64's developer mode on unconditionally, because four separate gates between the key and the window depend on it. `WR64_INSPECTOR=0` turns the port's half off. Idempotent, and chained into `patch_rt64.py` because the port links against the symbol. See [../docs/HUD-INSPECTOR.md](../docs/HUD-INSPECTOR.md). |
| `patch_rt64_rectlog.py` | Makes RT64 print where every rectangle actually lands on the widened framebuffer -- its own coordinates, origins, aspect flag and the resulting position and width. `WR64_RECT_LOG=1` switches it on. Idempotent. |
| `patch_rt64_eventfilter.py` | Makes RT64 take its SDL event filter back off when it shuts down. `ApplicationWindow::setup` chains itself into SDL's filter and the destructor never removes it, so SDL keeps a pointer to the destroyed object and the next event -- `SDL_DestroyWindow` pumps messages, so there is always one -- makes a virtual call through freed memory. That killed the process on every normal exit. Restores the previously installed filter rather than clearing, since RT64 chained onto it. Idempotent, and chained into `patch_rt64.py`. |
| `patch_rt64.py` | Patches RT64 so this game's inset frame is kept at 4:3 for the HUD and presented without its black borders. Idempotent. |
| `wsl_setup_splat.sh` | Prepares a Python environment for the vendored splat. |
| `wsl_run_splat.sh` | Disassembles the dump with the config written for it. |
| `wsl_run_splat_asmonly.sh` | Produces an assembly-only disassembly of the dump. |
| `make_asm_only_yaml.py` | Derives the asm-only splat config from the decomp's config. |
| `fix_asmonly_ld.py` | Repoints three orphan data objects in the asm-only linker script. |
| `pad_data_objects.py` | Pads each generated object's data sections to their true length. |
| `pad_segment_tails.py` | Pads each segment out to its declared ROM length. |
| `wsl_build_elf.sh` | Assembles the disassembly into an ELF with symbols. |
| `wsl_verify_elf.sh` | Checks the assembled ELF is faithful to the ROM. |
| `wsl_build_all.sh` | The whole pipeline above, ROM to verified ELF, in order. |
| `wsl_build_recompiler.sh` | Builds the N64Recomp and RSPRecomp tools under Linux. |
| `jal_scan.py` | Finds call targets that carry no function symbol, for the recompiler config. |
| `wsl_recompile.sh` | Runs the recompiler and regenerates the declarations header. |
| `wsl_recompile_rsp.sh` | Recompiles the audio microcode. |
| `gen_reimplemented_decls.py` | Declares the libultra functions the runtime reimplements. |
| `gen_runtime_func_table.py` | Registers runtime-provided libultra functions in the address lookup. |
| `fix_overlay_relocs.py` | Drops the relocation entries N64Recomp emits with no type. |
| `package_release.ps1` | Stages a built Windows tree into a release folder and zips it. See the script for what it deliberately leaves out. |
| `package_release.py` | The same for Linux (`.tar.gz`) and macOS (`.zip` of the signed bundle), picking the platform it is running on. Refuses to package a dump or a save, and refuses to overwrite an existing archive. |

## Per-platform build scripts

Each of these is the whole thing from a clean clone, in the order
`docs/BUILDING.md` describes step by step.

| Script | Purpose |
|---|---|
| `setup_linux.sh` | Reports or installs the Linux build packages, initialises the submodules, clones the reference decompilation, and builds the Python environment splat runs in. `--install` to actually install. |
| `build_linux.sh` | Applies every patch script, builds the recompiler, generates the game sources from a dump on a first build, then configures and builds. |
| `setup_macos.sh` | The macOS equivalent, and additionally builds MIPS binutils from checksum-pinned source into `build-toolchain/`, since there is no formula worth relying on. |
| `build_macos.sh` | As `build_linux.sh`, then bundles and ad-hoc signs the `.app`. |
| `build_macos_dependencies.sh` | Builds SDL2, FreeType and libpng from pinned source against the bundle's deployment target, for a redistributable build. Homebrew's copies can require a newer macOS than the app targets. |
| `package_macos.py` | Copies a built bundle's non-system dylibs into `Contents/Frameworks`, rewrites their install names, derives `LSMinimumSystemVersion` from what is actually shipped, and signs from the inside out. Run by `build_macos.sh`. |
| `generate_game.py` | Verifies a dump (converting byte order if needed), then runs the whole phase 01/02/05 pipeline over it: disassemble, assemble, verify, recompile the game, recompile the audio microcode. Runs natively on Linux and macOS and through WSL on Windows. |
| `patch_macos.py` | Adds the `<stdlib.h>` that the pinned hlsl++ needs for `labs`, which only Apple's libc notices is missing. Idempotent, and chained into `patch_rt64.py`. |
| `toolchain.py` | Finds `mips-linux-gnu-readelf`: natively where there is one, through WSL on Windows. |

## Testing and diagnosis

| Script | Purpose |
|---|---|
| `capture_window.ps1` | Photographs the running port at intervals, in physical pixels. |
| `instrument_funcs.py` | Traces when specific recompiled functions run. |
| `wsl_check_pc16.sh` | Checks whether the `R_MIPS_PC16` relocations in the overlay sections are safe to discard. |
| `wsl_diag_asm.sh` | Explains why assembling splat's output fails, when it does. |
| `wsl_reloc_types.sh` | Lists which relocation types the assembled ELF contains. |
| `probe_delta.py`, `probe_layout.py`, `probe_piecewise.py` | Phase 01 measurements of how the Rev A segment map relates to the v1.0 dump, kept for the record. |
| `scripts/` | Timed input scripts for `WR64_INPUT_SCRIPT`; `race.txt` drives the game from boot into a race. |
