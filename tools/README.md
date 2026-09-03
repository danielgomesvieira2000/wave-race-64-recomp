# tools

Committed tooling. Everything here is original code that operates on a ROM the
user supplies; nothing here contains game data.

| Script | Phase | Purpose |
|---|---|---|
| `check_toolchain.ps1` | 00 | Reports which build tools are present on Windows. |
| `patch_rt64.py` | 06 | Patches RT64 so this game's inset frame is kept at 4:3 for the HUD and presented without its black borders. Idempotent; rerun after a submodule update. |
| `splat_to_syms.py` | 01 | *(planned)* Convert splat output into an N64Recomp symbol table, applying size corrections and libultra renames. |
| `jal_scan.py` | 01 | *(planned)* Scan `.text` for JAL targets splat did not classify as functions. |
| `fix_zero_loads.py` | 02 | *(planned)* Strip assignments to `$zero` from recompiler output. |
| `find_bad_labels.py` | 02 | *(planned)* Detect unresolved jump tables in recompiler output. |

The last four are the standard set every non-decompiled recomp project ends up
writing. Prior art worth reading before writing our own:
[GGA-Recomp/tools](https://github.com/dantheman11294/GGA-Recomp) and
[WACOMalt/WaveRace64-Recomp](https://github.com/WACOMalt/WaveRace64-Recomp).
