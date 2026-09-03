# Phase 02: first recompile

**Gate met.** 1,346 functions recompiled into 25 C files (~20 MB), compiled with
Clang into a 12.1 MB static library carrying 1,201 defined text symbols, and
linked into the executable.

```
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
      -DWR64_WITH_RECOMPILED=ON
cmake --build build
./build/WaveRace64Recomp.exe --version   ->  recompiled game code : linked
```

Reproduce the generation with:

```
wsl -d Ubuntu -- bash tools/wsl_build_recompiler.sh
wsl -d Ubuntu -- bash tools/wsl_recompile.sh
```

`recomp_overlays.inl` (132 KB) came out with per-section function tables, which
is the overlay dispatch data librecomp consumes. `relocatable_sections_path`
stayed enabled throughout, the setting both prior public attempts had to switch
off.

## Run the recompiler under Linux, not Windows

The Windows build dies with `0xC0000409`, a `__fastfail` and here a stack
overflow, partway through writing its output. It is a nasty failure because it
still writes 28 files and looks like it worked: `funcs.h` simply ends mid-token
(`void func_801DEC00(uint8`) and the last `funcs_*.c` ends `ctx->r2 = ct`. It is
caught only by compiling the result, or by checking that `funcs.h` ends in
`#endif`, which `tools/wsl_recompile.sh` now does.

Linux gives an 8 MB default stack against the 1 MB Windows default, and
completes cleanly.

## Four failures, four different causes

Each was a distinct class, and each error message pointed somewhere other than
the actual problem.

### 1. Direct calls into overlays cannot be resolved statically

`No function found for jal target: 0x802C744C`

`tools/jal_scan.py` found **exactly three** resident functions that `jal`
straight into the overlay window at `0x802C5800`, across 24 call sites:
`func_80092CF0` (20 targets, almost certainly the overlay dispatcher),
`func_800922E4` (3) and `PauseMenu_Update` (1).

This is by design, not a gap. `resolve_jal()` never treats a function in a
relocatable section as a candidate from another section, because which overlay
is resident is a runtime fact. The escape hatch,
`use_lookup_for_all_function_calls`, is a context field with no config key,
neither in our pinned revision nor upstream.

The three are stubbed so the other ~1,343 get through. **Phase 04 must
reimplement them as `RECOMP_PATCH` functions dispatching through the runtime
overlay lookup.** This is the single largest known gap in the port.

A first version of that scan reported 775 unresolved targets. Both extra causes
were mine: cross-section calls are perfectly normal, and scanning whole sections
decodes each segment data as instructions. The `aspMain`, `rspboot` and `f3d`
blobs are RSP microcode sitting inside the main_segment text range, and decoding
those as MIPS invents call sites. Scanning only inside function bodies gives 41;
resolving cross-section calls properly gives 24.

### 2. A linker-script assignment can override an object symbol

`No function found for jal target: 0x801ED338`

The splat-generated `undefined_funcs_auto.ld` contained
`func_801ED338 = 0x801ED338;`, but our assembly *defines* that function. A
linker script assignment wins, producing an `ABS` symbol with no section, and
N64Recomp cannot resolve a call to a symbol whose section is unknown.
`tools/fix_asmonly_ld.py` now drops any entry there that our objects define.
Exactly one was spurious; the other 22 are genuine overlay entry points.

### 3. Do not list functions the tool already handles

`Function __osExceptionPreamble is set as ignored in the config file but does
not exist!`

The message is misleading, because the function does exist. N64Recomp carries a
built-in `reimplemented_funcs` list, renames those to `<name>_recomp` and
delegates them to ultramodern, and the rename removes the original name from the
lookup the config check uses. Listing them is therefore an error, not a
precaution.

The genuine entry is `func_800CB0A8`, the tail of `__osException` that splat
split into a separate symbol. `libultra/exceptasm.s` is hand-written assembly
whose control flow crosses that boundary (`b .L800CB060` branches back inside
`__osException`), which is why the recompiler rejected it. Its seven neighbours
need no entry at all.

### 4. Three symbol lists produce `_recomp` calls, not one

The generated code called 67 `_recomp` functions; `funcs.h` declared 57. Under
C99 the rest are implicit declarations and the build fails.

`tools/gen_reimplemented_decls.py` emits declarations for them, wired in through
the `recomp_include` config option. Reading only `reimplemented_funcs` is not
enough and fails identically: `__osPfsSelectBank` and `__osContRamRead` come
from `ignored_funcs`, and `renamed_funcs` is a third source. All three are read,
giving 440 declarations. Declaring a name that is never called costs nothing;
missing one is a build error.

## Open items for later phases

- **Overlay dispatch** (phase 04). The three stubbed functions above. Nothing
  that reaches an overlay will work until they are reimplemented.
- `lookup.cpp` reports the ROM name as `wr64.z64`, derived from the ELF
  filename. Harmless now; check against what librecomp expects in phase 03.
- Four `[WARN] LO16 reloc ... follows LO16 with different symbol` in `.ovl_i0`
  and `.ovl_i2`. Not fatal, not yet investigated.
