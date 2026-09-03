# Phase 05 findings: audio

Phase 04 left the port silent by design. Two pieces were placeholders: the RSP
callback reported every audio task complete without running it, and the audio
callbacks discarded their samples. This phase replaced both.

## The audio path, end to end

The game mixes audio on the RSP, not the CPU. Its audio thread builds a list of
ABI commands each frame, hands it to the RSP as a task, and the RSP writes
finished 16-bit stereo samples back into RDRAM. The CPU then points the audio
interface at that buffer with `osAiSetNextBuffer`. So making the port audible
meant recompiling the cartridge's audio microcode and then putting the samples
it produces on a sound card.

`RSPRecomp` handles the first half. It is a separate tool from `N64Recomp`,
driven by `recomp/aspMain.rsp.toml`, and it emits C++ rather than C because the
generated code uses librecomp's RSP vector unit, which is a C++ header of SSE
intrinsics. The output is one function, `aspMain_run`.

## The microcode does not run at IMEM 0

This was the phase's real problem, and it is worth stating plainly because
nothing about it fails loudly.

`text_address` tells RSPRecomp where in the RSP's instruction memory the blob
executes, and every jump and branch is resolved against it. The obvious value is
`0x04001000`, the base of IMEM. It is wrong. `rspboot` occupies the first `0x80`
bytes and loads the task's microcode after itself, so the correct value is
`0x04001080`.

Set to `0x04001000`, the recompiler produces a complete, plausible-looking file
in which every jump lands `0x80` bytes early. It reports no error, and the
result compiles and links. What it did at runtime was subtler than a crash: the
microcode's first call -- the one that DMAs the command list into DMEM -- landed
in the middle of an unrelated routine, so the dispatcher read an empty command
buffer and spun forever inside the DMA loop.

That produced a freeze with no diagnostic at all. The RSP task thread never
returned, so the game's scheduler sat at "yield requested, RSP busy" waiting for
an SP-complete event that would never come, and stopped starting tasks of either
kind. Every other thread carried on: the audio thread kept building tasks that
were never dispatched, and the window kept swapping the same finished frame. The
port looked like it was running.

Three independent checks agree on `0x1080`, and any one of them would have
settled it:

- The microcode contains exactly three call targets (`0x1150`, `0x1184`,
  `0x11B0`). At `0x1080` all three land on the first instruction of a
  subroutine. At `0x1000` all three land mid-routine.
- The command jump table's first entry is the handler for the no-op command.
  At `0x1080` it resolves to the dispatch loop's own back-edge, which is exactly
  what a no-op should do.
- The table's entries span `0x1118`..`0x1E24`. Text placed at `0x1080` runs to
  `0x1EA0` and contains all of them; text placed at `0x1000` ends at `0x1E20`
  and does not contain the last.

## The command jump table is invisible to the recompiler

The microcode dispatches each audio command through `lh $2, 0x10($2)` followed
by `jr $2`: a table of sixteen halfwords at DMEM `0x10`, inside the microcode's
*data* segment rather than its text. RSPRecomp finds branch targets by walking
the instruction stream, so a target that exists only as data is invisible to it.
The first audio task fell through the generated switch and returned
`UnhandledJumpTarget`.

The sixteen values are read straight out of the cartridge at `0xA9320`..`0xA933F`
and listed in `extra_indirect_branch_targets`. They are the entry point of each
command handler.

## The RSP ignores the low two bits of a jump target

The RSP's program counter is twelve bits and instructions are word aligned, so
`jr` discards the low two bits of its register: a target of `0x12EF` means
`0x12EC`. RSPRecomp's generated dispatch switches on the raw value, which is
fine only while every target is already aligned. Wave Race's table is not always
aligned at the moment the game uses it.

`tools/patch_rsprecomp.py` masks the switch with `0x1FFC` instead of `0x1FFF`,
restoring the hardware's behaviour for every microcode. Like the other submodule
patches in `tools/`, it is scripted and idempotent, because a submodule update
would otherwise revert it silently and the port would go quiet again with no
obvious cause.

## The two channels arrive swapped

librecomp stores RDRAM byte-swapped so the `MEM_*` macros can read big-endian
N64 words as native little-endian ones, which it does by XORing the low bits of
every address. `ultramodern::queue_audio_buffer` hands out a raw pointer, which
skips that: within each 32-bit word the two 16-bit halves sit in the opposite
order to the cartridge's. Each word holds one left and one right sample, so the
audible result is the stereo image mirrored -- correct-sounding music with the
channels reversed, which is the kind of bug that survives casual listening.
`queue_samples` swaps them back.

## Output

SDL's queue API, not a pull callback. The interface ultramodern expects *is* a
queue: the game asks how much is still buffered and decides how much more to
generate from the answer. Mirroring that directly keeps the two in step, where a
callback would need its own ring buffer in between and a second place for the
sample count to drift.

The device is opened with `SDL_AUDIO_ALLOW_ANY_CHANGE` unset, so SDL converts
internally if the hardware disagrees. The sample rate is the one thing that must
not silently differ: the game paces itself against how fast the queue drains, so
a device running at 48000 while the game believes it is feeding 32000 would make
the whole audio thread run at the wrong speed. `set_frequency` reopens the
device; the game starts at 32000 and settles on 26900.

## What was added to diagnose it, and kept

A hang watchdog (`wr64::watch_for_hang`) wraps the microcode call. If it has not
returned after five seconds, a detached thread suspends the RSP task thread,
samples its instruction pointer repeatedly and resolves the distinct addresses
to source lines. A spin loop is otherwise the least visible failure this code
can have -- no fault, no output, no return -- and it is what the wrong load
address produced. Sampling repeatedly rather than once matters: with everything
inlined, a single sample usually names a helper rather than the loop.

`main` now leaves stdout unbuffered. Every run in this project is inspected by
redirecting output to a file, which makes stdout fully buffered, and the process
is normally killed rather than exiting -- so the buffer is discarded. The
recompiled microcode reports unhandled jump targets through `printf`, and those
reports were being lost entirely.

## Verifying menus and racing

Neither is observable from a terminal: a port stuck on the title screen and one
quietly racing look identical from outside, and both look like a window that is
up. Two additions make the answer checkable.

`src/testdrive.cpp` watches `gGameState`, whose address the ELF gives and whose
values the decomp names, and reports every change. A run then produces a
transcript -- title screen, main menu, rider select, course select, racing --
rather than something to infer from pixels. It also reads an optional script of
timed inputs from `WR64_INPUT_SCRIPT`, so a session is repeatable and can live
in the repository next to the thing it tests. Without that variable set nothing
is injected and the pad and keyboard behave normally.

`tools/capture_window.ps1` photographs the window at intervals, because the
transcript says which screen the game thinks it is on and cannot say whether it
is drawn correctly -- a menu that advances with nothing on it is a renderer
problem, not a game-logic one.

What they showed:

- The title screen advances to the main menu on Start, within one frame.
- Rider select, course select and the course overview each advance in turn, and
  the transcript names them.
- A championship race starts and runs: the HUD shows time, rank, lap and speed
  counting, with crowd, banners, palm trees, buoys, wake spray and the water
  surface all drawn. The water -- the documented HLE risk for this game -- is
  correct on the title screen and during racing.
- Audio plays throughout.

The window is now sized to the display rather than hardcoded. A fixed 2x of
640x480 is 1280x960, which is taller than a 1536x864 laptop panel; Windows then
places the window partly off-screen and the game is silently cropped, which is
exactly what the first captures showed before the geometry was measured.

## State at the end of the phase

A 100-second run reaches attract mode with audio, completing 5,500 audio tasks
and 1,875 graphics tasks with no unhandled jump targets and no hangs. The
scheduler cycles normally through its three states.

One defect is characterised but not fixed. It fires roughly every 30 seconds of
play -- more often than the "one run in three" an earlier draft of this document
claimed, which was measuring a rate-limited counter rather than the fault.

A vector store walks off the end of DMEM. Instrumenting every vector store in
librecomp's RSP caught it stepping `0xFF0`, `0x1000`, `0x1010`, `0x1020`,
`0x1030`; DMEM is 4KB, so everything from `0x1000` wraps to `0x000`, onto the
microcode's constant pool and the command jump table at `0x10`. The next command
dispatched jumps to a corrupted address.

What is established:

- The game's audio memory map gives each of its four channel buffers exactly 160
  samples, and puts the last one flush against `0xF80`. So anything writing past
  `0xF80` wraps onto the table, and the layout leaves no slack at all.
- The corrupted entries are always their correct value plus or minus something
  under 32, **in both directions**. That makes it a read-modify-write -- a mix --
  and rules out the scaling operation an earlier theory here assumed.
- The mixer loop was compared instruction by instruction against the cartridge
  and is faithful, including its software pipelining and its signed loop bound.
- The game's own audio parameters, read live, are `target 544/frame,
  ai buffer 528..560, 4 updates/frame, chunk 136 (128..144)`. A per-update chunk
  is capped at 144 against the 160 the layout allows, so the ordinary synthesis
  path cannot overrun -- and the reverb and final-interleave paths use hardcoded
  `0x140`/`0x280` lengths that fit exactly.
- librecomp's vector stores mask DMEM addresses exactly as the hardware does, so
  the wrap is faithful; what reaches it is wrong, not how it is applied.

**Update: the overrunning command is ENVMIXER, but not the way this document
first concluded.** `src/callbacks.cpp` carries a standing diagnostic
(`bisect_audio_task`, active for the first six dropped frames of a run) that
re-runs a failed task from a fresh DMEM with 1, 2, 3... of its commands,
comparing the command jump table (DMEM `0x10`-`0x2F`, the sixteen halfwords
`lh $2, 0x10($2)` indexes into) against a pristine copy after each, so the
culprit is whichever prefix first disturbs it.

That comparison originally covered the whole `0x00`-`0x40` window, which
turned out to be a mistake: part of that window is DMA chunk-remainder
bookkeeping that legitimately varies with how many bytes of the command list
get consumed, and comparing the whole window flagged that as "corruption,"
pointing at several innocent commands (a plain SETBUFF, a RESAMPLE) in turn.
Narrowing the comparison to just the table entries themselves was the actual
fix needed before trusting what it points at.

With that fixed, the command it always points at is **ENVMIXER**. The
recompiled handler (`RecompiledFuncs/aspMain_rsp.cpp`, label `L_1B38`) reads a
persistent 16-byte "voice state" block the microcode keeps at a fixed DMEM
address (register r24, fixed at `0x360` for the whole task) to decide where to
write its mixed samples, when its flags byte has bit 3 set -- with the bit
clear, the handler instead forces a fixed, safe scratch address (`r23 + 0x50`,
r23 fixed at `0xF90`), ignoring the state block entirely.

Instrumenting every store into the table directly (rather than inferring the
address from the surrounding command dump) confirmed the corrupting write is
a 16-byte vector store landing at `0x1010`-`0x102F`, wrapping DMEM's 4KB the
same way the original stepping (`0xFF0`, `0x1000`, `0x1010`, `0x1020`,
`0x1030`) suggested. But the failing instance had flags `0x09` -- bit 3 *and*
bit 0 both set -- not `0x08`, and critically, no aux-mode SETBUFF (the one
that refreshes the state block's write-pointer field) had run anywhere in the
preceding ~80 commands. Meanwhile, directly watching that write pointer across
many genuine `SETBUFF f=08 / ENVMIXER f=08` pairs -- the ordinary, paired case
-- never once showed it approaching DMEM's end; the values stayed in the
`0x0A80`-`0x0E40` range throughout.

That points at a stale value rather than a single command's arithmetic
overrunning: ENVMIXER's explicit-buffer mode trusts whatever the state block
currently holds without checking whether an aux SETBUFF set it recently, and
if the game ever issues that flag combination without one immediately
preceding it -- reusing a much older buffer address left over from a
different, unrelated voice -- the value has no relationship to the current
mix at all. What still needs pinning down: whether the game's own command
stream ever legitimately does this on purpose (relying on state a much
earlier SETBUFF left behind), or whether some earlier aux SETBUFF simply never
got the chance to run when it should have -- and if so, why not. That is the
thread to pull next.

Until then `asp_main_watched` reports the task complete instead of letting
librecomp assert, which costs one frame of audio -- a click -- rather than the
run. That is a net, not a fix, and it says so on stderr every time it catches
something.

Still to do for this phase: play every course in both directions, stunt mode,
and a full championship to the ceremony.
