# Phase 04: boot bring-up

In progress. The game runs, creates its threads, and submits its first display
list, which RT64 processes and returns from. It then faults on a null pointer in
the graphics code.

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
