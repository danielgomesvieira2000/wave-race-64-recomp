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
| `promote_water_sun.py` | Copies the sun directions saved from the F1 water sun editor (`water_sun.json` in the settings folder) into `assets/water/profiles.json`, keeping the file's formatting. `--dry-run`, `--file`, `--clear`. See [../docs/WATER.md](../docs/WATER.md), *Tuning the sun*. |
| `match_texture_pack.py` | Turns a Dolphin-named texture pack (the Wii Virtual Console release's `tex1_WxH_hash_fmt.png`) into a mod directory by matching each image to an RT64 dump texture by picture -- by colour, then by shape for redrawn art, allowing for the Virtual Console padding textures to multiples of 4 -- with a per-image report and review contact sheets. See [../docs/TEXTURE-PACK-MATCHING.md](../docs/TEXTURE-PACK-MATCHING.md). |
| `rt64_texture_dump.py` | Decodes an RT64 texture dump (`*.v5.tile.json`, `*.rice.*`) into RGBA images: size and row stride transcribed from RT64's Rice hasher, RDRAM word-swapped back to the N64's byte order. Importable; run directly it writes every texture as a PNG. |
| `pack_mod.py` | Zips a mod directory into the `.nrm` the game installs, checking the required `mod.json` fields first and naming the file from the mod's id and version. `--install` writes it straight into the game's mods folder. |
| `patch_rt64_texturepacks.py` | Adds `RT64_SetTexturePacks` to RT64 so the port can hand it a list of texture packs to load; RT64 otherwise only accepts one through a file dialog in its developer UI. Applied on the next frame, because a mod can be enabled from another thread while the game runs. Idempotent, and chained into `patch_rt64.py`. |
| `patch_rt64_inspector.py` | Two patches about the debug menu. Adds one function pointer to RT64 (`RT64_PortInspectorHook`) that it calls once per frame from `State::inspect()` with an ImGui frame open, so the port can draw its own window inside RT64's developer UI -- null unless the port sets it -- and unbinds F2, whose session-wide ray tracing toggle is not something to leave under a finger once developer mode is on for everyone (F1, F3 and F4 stay). F1 opens it, in every build: the port turns RT64's developer mode on unconditionally, because four separate gates between the key and the window depend on it. `WR64_INSPECTOR=0` turns the port's half off. Idempotent, and chained into `patch_rt64.py` because the port links against the symbol. See [../docs/HUD-INSPECTOR.md](../docs/HUD-INSPECTOR.md). |
| `patch_rt64_rectlog.py` | Makes RT64 print where every rectangle actually lands on the widened framebuffer -- its own coordinates, origins, aspect flag and the resulting position and width. `WR64_RECT_LOG=1` switches it on. Idempotent. |
| `patch_rt64_eventfilter.py` | Makes RT64 take its SDL event filter back off when it shuts down. `ApplicationWindow::setup` chains itself into SDL's filter and the destructor never removes it, so SDL keeps a pointer to the destroyed object and the next event -- `SDL_DestroyWindow` pumps messages, so there is always one -- makes a virtual call through freed memory. That killed the process on every normal exit. Restores the previously installed filter rather than clearing, since RT64 chained onto it. Idempotent, and chained into `patch_rt64.py`. |
| `patch_rt64.py` | Patches RT64: the inset frame kept at 4:3 for the HUD and presented without its black borders, the pairing log, per-view camera regions and the pairing jump limit; then chains the inspector, texture-pack, event-filter, macOS and water patches. Idempotent, and the single command for all of them except `patch_rt64_rectlog.py`, which is applied by hand. |
| `patch_rt64_water.py` | Adds the modern water renderer to RT64: a renderer, an interpolation header, a shared parameter block and ten shaders, threaded through twenty existing files. The one patch here that is a diff (`patches/rt64-water.patch`) rather than anchored strings, because at ~1200 lines of edits transcribing it into anchors would only add a class of error; `git apply` refuses a tree it does not match, which is the property the anchored scripts exist for. Two small anchored edits follow it: a transform guard, and the tag that gives Original water the same world-XZ interpolation as Enhanced (`WR64_WATER_INTERP_STATS=1` reports its cost). Idempotent, and chained into `patch_rt64.py` -- the port does not compile without the headers it adds, whatever the setting. See [../docs/WATER.md](../docs/WATER.md). |
| `patch_runtime_shutdown.py` | Joins ultramodern's game and timer workers before the shutdown path releases the queues and RDRAM they may still be reading. Fixes an intermittent crash on exit that survives the SDL event-filter fix. Also a diff, and idempotent the same way. |
| `wsl_setup_splat.sh` | Prepares a Python environment for the vendored splat. |
| `wsl_run_splat.sh` | Disassembles the dump with the config written for it. |
| `wsl_run_splat_asmonly.sh` | Produces an assembly-only disassembly of the dump. |
| `make_asm_only_yaml.py` | Derives the asm-only splat config from the decomp's config. |
| `fix_asmonly_ld.py` | Repoints three orphan data objects in the asm-only linker script. |
| `pad_data_objects.py` | Pads each generated object's data sections to their true length. Preprocesses with `clang -E`, as the ELF build does, and stops on any object it cannot measure. |
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
| `package_release.ps1` | Stages a built Windows tree into a release folder and zips it, with the third-party license texts in `licenses/`. See the script for what it deliberately leaves out. |
| `third_party_licenses.txt` | The license texts every release ships in `licenses/`: a name and a path per line, read by both packaging scripts. Keep it in step with [../THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md). |
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
| `package_macos.py` | Copies a built bundle's non-system dylibs and frameworks (resolving `@rpath`, `@loader_path`, `@executable_path`) into `Contents/Frameworks`, rewrites their install names, derives `LSMinimumSystemVersion` from what is actually shipped, and signs from the inside out. Run by `build_macos.sh`. |
| `generate_game.py` | Verifies a dump (converting byte order if needed), then runs the whole phase 01/02/05 pipeline over it: disassemble, assemble, verify, recompile the game, recompile the audio microcode. Runs natively on Linux and macOS and through WSL on Windows. |
| `patch_macos.py` | Adds the `<stdlib.h>` that the pinned hlsl++ needs for `labs`, which only Apple's libc notices is missing. Idempotent, and chained into `patch_rt64.py`. |
| `toolchain.py` | Finds `mips-linux-gnu-readelf`: natively where there is one, through WSL on Windows. |

## Reverse engineering

| Script | Purpose |
|---|---|
| `decode_water_field.py` | Decodes the game's wave field (`D_80162420`, 384x128 cells of `{s16 height, s16 age}` on a 64-unit triangular lattice) and checks the decode against the game's own height query, which `WR64_WATER_FIELD` probes on a grid of thousands of points in the same frame. Reading the code gives a model; reproducing the game's own answers is what makes it usable. `--image` writes a PGM of the field. See [../docs/GAME-INTERNALS.md](../docs/GAME-INTERNALS.md), *The wave field*. |

## Testing and diagnosis

| Script | Purpose |
|---|---|
| `capture_window.ps1` | Photographs the running port at intervals, in physical pixels. |
| `pairing_log.py` | Reads the pairing log RT64 writes under `WR64_PAIRING_LOG`. By default the object pairs: the jump distribution and the frames with a pair further apart than a limit. `--scenes`: camera pairs that crossed framebuffer slots or screen regions, the 2P burst's signature. `--models`: transforms grouped into models by position, with frames where a moving model had a part unpaired or snapped -- a rider coming apart (a model's count can include one adjacent non-part, such as a shadow). `--calls`: object pairs by draw-call hash. See [../docs/TRANSFORM-PAIRING.md](../docs/TRANSFORM-PAIRING.md). |
| `capture_frames.py` | Launches the port and saves every frame its window presents between two times, even with another window on top -- Windows Graphics Capture, not a desktop grab. 30-40 frames a second at half size, named by milliseconds since launch, with the launch time recorded to line up with the pairing log. `--env` sets variables for one run. Windows only; `pip install windows-capture opencv-python`. |
| `contact_sheet.py` | Tiles a span of captured frames into one labelled image, optionally cropped -- to one split-screen view, say -- so consecutive frames can be compared at a glance. Needs Pillow. |
| `instrument_funcs.py` | Traces when specific recompiled functions run. |
| `wsl_check_pc16.sh` | Checks whether the `R_MIPS_PC16` relocations in the overlay sections are safe to discard. |
| `wsl_diag_asm.sh` | Explains why assembling splat's output fails, when it does. |
| `wsl_reloc_types.sh` | Lists which relocation types the assembled ELF contains. |
| `probe_delta.py`, `probe_layout.py`, `probe_piecewise.py` | Phase 01 measurements of how the Rev A segment map relates to the v1.0 dump, kept for the record. |
| `render_distance_census.py` | Turns a `WR64_DISTANCE_CSV` capture into per-kind cull verdicts. See [../docs/RENDER-DISTANCE-CENSUS.md](../docs/RENDER-DISTANCE-CENSUS.md). |
| `lattice_shift.py` | Tests from a `WR64_LATTICE_VERTS` trace whether the water lattice carries its heights or resamples a world-fixed field. The trace records every water vertex's position and texture coordinates; `WR64_LATTICE_VERT_TRIGGER=<file>` starts it on the first frame after that file exists, so a capture can be taken at a place someone drove to by hand. |
| `scripts/` | Timed input scripts for `WR64_INPUT_SCRIPT`; `race.txt` drives the game from boot into a race, `race-course2.txt` into a race on the second course, `race-2p.txt` both players into a 2P VS race, `rider-select.txt` to the watercraft select screen to switch riders back and forth. Buttons prefixed `2:` are player two's. |
