# Phase 04: boot bring-up

**The game boots and renders.** The Nintendo 64 logo appears, correctly shaded
and animated, drawn by RT64 from the cartridge's own display lists. The process
runs indefinitely instead of crashing: 45 threads, a steady frame loop, and a
window titled "Wave Race 64: Recompiled".

Neither existing public port reaches this. One shows "two boxes of noise"; the
other has never been launched.

The sections below record how it got there, starting from the crash that was
blocking it.

## Where it gets to

```
[wr64] runtime initialised; entering recomp_entrypoint
[wr64] rdram base = 000001CF35FF0000
Initializing recomp heap at offset 0x01000000 with size 0x1F000000
[wr64] game thread 1..4 created
[wr64] main loop tick 0
[wr64] update_screen #0
[wr64] first display list: ucode 0x800D2380 data 0x800EE310 dl 0x801388D0
[wr64] send_dl returned from the first display list
[wr64] ==== CRASH ====
[wr64] ACCESS_VIOLATION while writing address 0x1CFB5FF0004
[wr64] in function SysMain_GfxFullSync + 0xF5
[wr64] at RecompiledFuncs/funcs_13.c:7873
```

For comparison, the two existing public Wave Race 64 ports reach "two boxes of
noise" and "builds but never launched". This one submits a real display list
with the game's own F3D microcode.

## The crash: gDisplayListHead is null

The faulting address minus the RDRAM base is **exactly `0x80000004`**, and that
number identifies the bug on its own.

`MEM_W` does no masking:

```c
#define MEM_W(offset, reg) \
    (*(int32_t*)(rdram + ((((reg) + (offset))) - 0xFFFFFFFF80000000)))
```

`0xFFFFFFFF80000000` is -0x80000000 as a signed 64-bit value, so the macro adds
`0x80000000` to a register that already holds a KSEG0 address, cancelling it
out. Working backwards from `rdram + 0x80000004` for `MEM_W(0x4, reg)` gives
`reg + 4 + 0x80000000 == 0x80000004`, so **`reg` was zero**.

The source confirms what that register is. `SysMain_GfxFullSync` appends a
full-sync command to the display list:

```c
// 0x80046BF8: addiu  $a1, $a1, 0x1944      -> $a1 = 0x80151944, gDisplayListHead
// 0x80046BFC: lw     $v1, 0x0($a1)         -> $v1 = *gDisplayListHead   == 0
// 0x80046C0C: sw     $t6, 0x0($a1)
// 0x80046C10: sw     $zero, 0x4($v1)       <- faults, $v1 is null
```

`0x80151944` is `gDisplayListHead` in the decomp's symbol corpus, so this is not
a guess about which pointer it is. The game read its display list head, got
null, and wrote through it.

Note the fault is the runtime working correctly, not failing. librecomp
deliberately allocates RDRAM inside a much larger `PAGE_NOACCESS` region so an
invalid game pointer faults immediately instead of silently corrupting memory.

## What to investigate next

Something that should have initialised the display list buffer did not run. The
three functions stubbed in phase 02 are the obvious suspects, since
`func_80092CF0` alone dispatches to 20 overlay entry points and stubbing it
means whole subsystems never execute. That would need confirming rather than
assuming: the first step is to find what writes `0x80151944` and check whether
it is reached.

## Tools added

- `src/crash_handler.cpp` -- a vectored exception handler reporting the faulting
  address, the address being accessed, the module, and, via dbghelp, the
  function name and source line. This is what turned "it exits" into
  "SysMain_GfxFullSync + 0xF5 at funcs_13.c:7873". Worth having permanently:
  the recompiled code is linked into the executable, so without symbol
  resolution every crash reports the same unhelpful module.
- `WR64_SKIP_DL` -- an environment variable that makes `send_dl` skip RT64's
  display list processing, to separate a fault inside RT64 from one anywhere
  else. It is what established that RT64 was not at fault here.

## Two measurement traps worth remembering

**`cmd /c "prog & echo %errorlevel%"` reports the wrong value.** cmd expands
`%errorlevel%` when it parses the line, before the program runs, so a hard
crash read back as a clean `EXITCODE=0`. That cost real time: an access
violation was recorded as an orderly shutdown, and the investigation went
looking for who had called `quit()`. Use `cmd /v:on` with `!errorlevel!`, or
invoke the program directly from PowerShell and read `$LASTEXITCODE`.
PowerShell's `Start-Process -PassThru` also returned an empty `ExitCode` here.

**A virtual address is not an RDRAM offset.** `send_dl` originally passed the
OSTask's display list pointer to RT64 unmasked. RT64 indexes straight off the
RDRAM base, so `0x801388D0` indexed two gigabytes past an 8 MB allocation.
`physical()` strips the segment bits. This was not the cause of the current
crash, but it was a real bug on the same path.

---

# The null pointer, solved

The cause was the phase 02 stub, and the chain is exact rather than inferred.
In `SysMain_Thread`:

```
800471FC: jal func_80092CF0
80047200:   lw $a0, %lo(gDisplayListHead)($a0)   ; delay slot: head passed in
80047208: jal SysMain_GfxFullSync
8004720C:   sw $v0, %lo(gDisplayListHead)($at)   ; delay slot: return stored back
```

`func_80092CF0` takes the display list head, appends what the resident overlay
draws, and returns the advanced pointer -- which is written straight back into
`gDisplayListHead`. Phase 02 stubbed it, because it `jal`s into the overlay
window and cannot be resolved statically. An empty stub never sets `v0`, so the
head became null and the next `SysMain_GfxFullSync` wrote through it.

Watching the value across calls showed it precisely:

```
GfxInitBuffers #1: [0x80151944] = 0x00000000   (before it initialises)
GfxFullSync    #1: [0x80151944] = 0x80138928   valid
CreateGfxTask  #1: [0x80151944] = 0x80138938   valid
GfxInitBuffers #2: [0x80151944] = 0x80138938   valid
GfxFullSync    #2: [0x80151944] = 0x00000000   <- nulled in between
```

`patches/overlay_dispatch.cpp` now defines `func_80092CF0` to return its
argument unchanged: nothing is drawn, the pointer stays valid, the frame
completes. **That is an interim, not the fix** -- everything those 20 overlay
entry points draw is still missing, which is why the logo appears but nothing
beyond it will.

## `ignored`, not `stubs`, for a patched function

`RECOMP_FUNC` is `extern inline __attribute__((weak, noinline))` under Clang --
and clang-cl takes that branch, because the MSVC branch is guarded on
`!defined(__clang__)`. A strong definition should therefore win.

On PE/COFF it does not, if the function was stubbed. The stub is emitted into
the recompiled library, and the archive member holding it gets pulled in for the
*other* functions sharing its object file, so both definitions reach the link
and lld-link reports a duplicate symbol.

Moving the function from `stubs` to `ignored` fixes it properly: `ignored`
emits nothing at all, leaving the patch as the only definition. Its callers
still need a declaration, so `tools/gen_reimplemented_decls.py` now also
declares whatever `recomp/wr64.toml` marks `ignored`.

## Tooling

`tools/instrument_funcs.py` inserts a one-shot `printf` at the entry of named
recompiled functions, or with `NAME@0xADDR` prints an RDRAM word on every call.
Order alone answers "did it run"; a watched value answers "and did it stay
valid", which is the question once an initialiser is known to have run before
its user. It edits generated code, which is safe only because re-running the
recompiler erases the edits.

---

# Real overlay dispatch

The interim patch is gone. All three functions stubbed in phase 02 --
`func_80092CF0`, `func_800922E4` and `PauseMenu_Update` -- are now recompiled
properly, and their calls into overlays resolve at runtime.

## What func_80092CF0 actually is

Reading it settled the approach. It is the game-state renderer dispatcher: it
appends a display list command, then jump-tables on `gGameState` through
`jtbl_800EAFA8`, 104 entries wide, and each case calls the draw function for
that state -- many of them directly into overlays:

```
80092D60: sltiu $at, $t7, 0x68        ; gGameState < 104
80092D74: lw    $t7, %lo(jtbl_800EAFA8)($at)
80092D78: jr    $t7
...
80092DC0: jal   func_802C5BA4          ; straight into the overlay window
```

Hand-transcribing a 302-instruction dispatcher with a 104-entry jump table --
and its two companions -- would have been far more code than the alternative,
and far more to get subtly wrong.

## Exposing the option the tool already had

`Context::use_lookup_for_all_function_calls` makes every call resolve by address
against whatever is currently loaded, which is exactly the semantics an overlay
needs. The field and the code path in `resolve_jal` both exist; there is simply
no config key for it, here or upstream, so the CLI can never set one.

`tools/patch_n64recomp.py` adds the key -- three insertions following the
pattern `trace_mode` already establishes. It is scripted and idempotent because
it patches a submodule: a submodule update would otherwise revert it silently
and the port would stop building for no visible reason.

## Turning it on breaks two assumptions

Both are the same shape: things that never needed an address before suddenly do.

**Runtime-provided libultra functions.** N64Recomp renames them to
`<name>_recomp` and never puts them in a section table, because with direct
calls nothing looked them up. The first call failed with `Failed to find
function at 0x800C6300` -- `osDpSetStatus`, which librecomp does implement.
`tools/gen_runtime_func_table.py` pairs each with the address it had on the
cartridge; 73 are registered.

The list comes from scanning librecomp and our own sources for actual
`<name>_recomp` *definitions*, not from N64Recomp's 440 names. Declaring an
unused function is free; taking its address forces the linker to find a
definition, and most of those names have none here.

**Resident sections.** `init_overlays()` does not populate the function map at
all -- it only records where sections live. Functions are added by
`load_overlay()` when the game DMAs a section in, which is sufficient when only
overlays are looked up by address. With every call a lookup, `main_segment` and
`codeseg` are never registered, because they are never loaded through PI DMA.
All 1,088 of their functions are now registered up front.

Two ordering traps here, both of which produce silence rather than an error:

- This must run from the game's `on_init` hook. `init_overlays()` begins with
  `func_map.clear()`, so anything registered before it is discarded, and
  librecomp calls it long before `on_init_callback`.
- A section is identified as an overlay by its **address being shared**, not by
  consulting `overlay_sections_by_index`. That table's values are not section
  indices -- they run 3..21, while `main_segment` is index 8 and `codeseg` is
  11 -- so using it as an index set silently skips most of the game. That
  mistake registered 150 functions instead of 1,088 and looked plausible.

## Where it stands

The failure has moved to `Failed to find function at 0x802C5800` -- the overlay
window itself. That is the correct next problem rather than a regression:
resident code now resolves completely, and what remains is that no overlay has
been registered, because nothing loads one.

librecomp calls `load_overlays` exactly once, for the boot region:

```c
load_overlays(0x1000, (int32_t)entrypoint, 1024 * 1024);
```

Everything after that is the game's job, and in this game it goes through
`game_dma_copy(rom, ram, size)` -- called from seven sites including
`GameLoad_LoadCodeseg` and `SysMain_Thread`. Driving `load_overlays` from there
is the remaining work, and it is the same conclusion the phase 00 plan reached
from the other direction: the overlay loader has to be wired to the runtime
through a patch.

---

# Announcing loaded code to the runtime

With every call resolved by address, the runtime has to be told when the game
brings code in, or the call fails. librecomp announces only the boot region:

```c
load_overlays(0x1000, (int32_t)entrypoint, 1024 * 1024);
```

Everything else is the game's business. Two paths matter, and finding the second
took a wrong turn worth recording.

## game_dma_copy is not the overlay path

`game_dma_copy(rom, ram, size)` is the obvious candidate: seven call sites,
including `GameLoad_LoadCodeseg` and `SysMain_Thread`. Patching it worked, and
tracing every transfer proved it was the wrong hook:

```
[wr64-dma] #1 rom 0x0A95D0 -> ram 0x801DAFA0 size 0x4CAC0   codeseg
[wr64-dma] #2 rom 0x0F6090 -> ram 0x80228E10 size 0x8290
[wr64-dma] #4 rom 0x374100 -> ram 0x802A0000 size 0x2800    asset chunks
...
```

`codeseg` loads through it exactly as expected, and **nothing ever lands at
0x802C5800**. `GameLoad_LoadOverlay` does not use it: it reads an entry from
`gOverlayTable` and calls `osPiStartDma` directly, with the destination
hardcoded.

The table entries are eight words:
`{romStart, romEnd, textStart, textEnd, dataStart, dataEnd, bssStart, bssEnd}`.

So the hook belongs one level down, at `osPiStartDma`, where both paths meet.
`patches/dma.cpp` wraps librecomp's own implementation rather than replacing it,
and `src/overlays.cpp` registers the wrapper at `osPiStartDma`'s cartridge
address in place of the runtime's.

## Two address conventions, both easy to get wrong

- `game_dma_copy`'s `a1` is a **physical** address -- the original passes it
  through `osPhysicalToVirtual` before the DMA. `do_rom_read` writes through
  `MEM_B`, which expects KSEG0, so handing the physical address straight over
  faults two gigabytes past RDRAM.
- The ROM argument is normalised the way librecomp does it, `(addr |
  rom_base) & 0x1FFFFFFF`, which accepts a bare file offset or a K1 cartridge
  address without needing to know which the caller used. `load_overlays` then
  wants the file offset, since that is the space the section table uses.

## Where it stands

Still `Failed to find function at 0x802C5800`, and the reason is now visible in
`SysMain_Thread`'s loop rather than mysterious:

```
800471FC: jal func_80092CF0        ; renderer, dispatches into overlays
80047208: jal SysMain_GfxFullSync
8004727C: jal game_dma_copy
80047290: jal unk_game_load
800472A4: jal GameLoad_LoadOverlay ; loads the overlay -- at the end
```

The renderer runs **before** the loader. On hardware that is fine: the load is
conditional, and an overlay requested on one iteration is used on the next. Here
the very first dispatch already targets an overlay that nothing has loaded, so
either `gGameState` starts at a value whose renderer lives in an overlay, or
something earlier in the sequence -- `func_800922E4` has three overlay targets,
and `unk_game_load` is unexamined -- is meant to have loaded it already.

That is the next thing to determine, and it is a question about the game's
startup sequence rather than about the recompilation.

## What unk_game_load does, and who really calls the overlay

`unk_game_load` is a 630-instruction dispatcher on `gGameState`, jump-tabling
through `jtbl_800EB150` over 72 entries. It stages what the next state needs --
course data, asset pointers -- and returns whether an overlay load is required.
`SysMain_Thread` uses that return to gate `GameLoad_LoadOverlay`:

```
80047290: jal  unk_game_load
8004729C: beqz $v0, .L800472AC     ; skip the load if nothing was requested
800472A4: jal  GameLoad_LoadOverlay
```

So it is not itself a loader; it decides whether loading is needed.

The call that fails is not in the renderer at all. Searching the generated code
for the address shows a single caller:

```
LOOKUP_FUNC(0x802C5800)  ->  func_800922E4
```

which runs *before* `func_80092CF0` in the loop, at `0x800471E0`. It is the
per-frame update: controllers, then a state machine. Its overlay calls are
guarded on `D_801CE638`, a screen-state variable:

```
80092424: lw   $v0, %lo(D_801CE638)($v0)
8009242C: addiu $at, $zero, 0x1
80092434: bnel $v0, $at, ...        ; == 1  -> overlay
80092470: bnel $v0, $at, ...        ; == 8  -> overlay
8009248C: bne  $v0, $at, ...        ; == 0x15 -> overlay
```

## The actual problem: gGameState never leaves 0

Tracing both variables gives the shape of it. `gGameState` reads 0 on every
iteration, while `D_801CE638` moves -- it was 0x11 on the first update and 0 on
the second.

That combination is the bug. `GameLoad_LoadOverlay` indexes its table by
`gGameState - 1` and checks the result unsigned against 0x66, so **state 0 loads
nothing at all** -- deliberately, since 0 is not a state with an overlay. The
screen state meanwhile advances to one whose handler *is* in an overlay, and
calls it. On hardware that never happens, because `gGameState` would have moved
first and the overlay would have been loaded on the way.

So the remaining question is not about overlays or dispatch, both of which now
work. It is why the boot sequence never advances `gGameState` past 0. The
stubbed audio and RSP microcode are the obvious suspects: if the game waits on
something audio-related before advancing, a silent stub that reports success
without doing the work would hold it at state 0 indefinitely while the rest of
the frame loop keeps running -- which is exactly the behaviour observed.
