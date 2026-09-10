# tools

Committed tooling. Everything here is original code that operates on a ROM the
user supplies; nothing here contains game data. The `wsl_*.sh` scripts run
under WSL because the disassembler and the MIPS assembler are Linux tools; the
rest run wherever Python or PowerShell does.

`docs/BUILDING.md` says which to run and in what order. This is the index.

## Building the port

| Script | Purpose |
|---|---|
| `check_toolchain.ps1` | Reports which build tools are present on Windows. |
| `patch_n64recomp.py` | Exposes N64Recomp's `use_lookup_for_all_function_calls` as a config option, which overlay dispatch needs. Idempotent. |
| `patch_librecomp.py` | Makes librecomp's function-lookup failures report the address they failed on. Idempotent. |
| `patch_rsprecomp.py` | Makes RSPRecomp's indirect jumps ignore the low two bits of the target, as the hardware does. Idempotent. |
| `patch_recompinput.py` | Adds `players::auto_assign_controllers` to RecompFrontend, so the port can put the first pad on player one and the second on player two without the assignment modal. Idempotent. |
| `patch_rt64_inspector.py` | Adds one function pointer to RT64 (`RT64_PortInspectorHook`) that it calls once per frame from `State::inspect()` with an ImGui frame open, so the port can draw its own window inside RT64's developer UI. Null unless the port sets it. F1 opens it, in every build: the port turns RT64's developer mode on unconditionally, because four separate gates between the key and the window depend on it. `WR64_INSPECTOR=0` turns the port's half off. Idempotent, and chained into `patch_rt64.py` because the port links against the symbol. See [../docs/HUD-INSPECTOR.md](../docs/HUD-INSPECTOR.md). |
| `patch_rt64_rectlog.py` | Makes RT64 print where every rectangle actually lands on the widened framebuffer -- its own coordinates, origins, aspect flag and the resulting position and width. `WR64_RECT_LOG=1` switches it on. Idempotent. |
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
| `package_release.ps1` | Stages a built tree into a release folder and zips it. See the script for what it deliberately leaves out. |

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
