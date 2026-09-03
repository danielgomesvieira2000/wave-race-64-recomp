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
