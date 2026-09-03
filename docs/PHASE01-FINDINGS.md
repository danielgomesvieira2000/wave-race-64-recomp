# Phase 01 findings: does the Rev A map fit v1.0?

The revision choice made in phase 00 rested on one assumption: that Wave Race 64
v1.0 and Rev A share a link layout, so the Rev A symbol corpus could be ported
across rather than rediscovered. `docs/PLAN.md` said phase 01 had to measure
that rather than carry it further. This is the measurement.

Reproduce with:

```
python tools/probe_layout.py rom.z64
python tools/probe_delta.py rom.z64
python tools/probe_piecewise.py rom.z64
```

## Answer

**The assumption is half right, and the half that fails is the important half.**

The revisions are unmistakably the same codebase — the correspondence is far too
strong to be coincidence — but they are *not* related by a single constant
offset. Rev A inserted code at several points, and the offset between the two
images changes across the address space. Rev A addresses therefore cannot be
used directly, and cannot be fixed by adding one number.

## Evidence

### The data region shifts by 0x240

Our v1.0 dump contains 131 MIO0-compressed blocks, the first at ROM `0x1AE420`.
The Rev A config declares its first MIO0-bearing segment at `0x1AE660`. Our data
region therefore sits `0x240` (576) bytes earlier: Rev A is the larger image,
as the later revision should be.

### Rev A code addresses mostly do not land on v1.0 function boundaries

Of the 221 Rev A `func_<hex>` symbols — every one a function by construction,
since the name encodes its own address — only 7 (3.3%) land on a defensible
function boundary in our dump when used unmodified. `func_801DAFA0` is
`codeseg`'s first function; at that ROM offset our dump holds `addiu $v1, $v1, 1`,
which is mid-function.

A naive version of this test, "do Rev A symbols sit on an `addiu $sp` prologue",
returns 2% and means nothing: most of the 853-symbol corpus names data, and leaf
functions have no such prologue. The `func_<hex>` subset is the only clean
denominator available, and boundary evidence is taken from the two words
*before* a candidate address — a preceding `jr $ra` plus delay slot — which is
much stronger than inspecting the candidate word itself.

### The shift is piecewise, and the pieces are real

Scanning candidate offsets for `codeseg` gives a sharp spike at `-0x2A0`, 117 of
213 symbols (54.9%) against a 1.9% background across 8,193 candidate deltas — a
29x signal. Resolving the remainder into regions:

| vram range | delta | symbols |
|---|---:|---:|
| `0x801DB430` – `0x801E3EE0` | `-0x2a0` | 86 |
| `0x801E4C08` – `0x801E6F6C` | `-0x2a0` | 12 |
| `0x801E71A8` – `0x801E8B24` | `-0x2a0` | 13 |
| `0x801E92FC` – `0x801EB180` | `-0x278` | 15 |
| `0x801EB4F4` – `0x801F25E0` | `-0x270` | 70 |
| `0x801F4120` – `0x801FC840` | `-0x248` | 15 |

Seven regions account for 217 of 221 `codeseg` function symbols. `main_segment`
shows the same shape with a coherent core — runs of 126, 73, 51 and 25
consecutive symbols each holding one exact delta in the `-0x240` to `-0x2A0`
band.

A run of 126 consecutive symbols agreeing on a single offset is not something
chance produces. These regions are real.

## What is not established

The per-symbol region assignment produced by `probe_piecewise.py` **overfits**.
It sweeps a ±0x800 delta window, so it can "explain" any address, and an early
version of this analysis reported 55 regions covering 100% of symbols — an
artifact, not a result. Only long runs are evidence. Short runs, and the odd
deltas that come with them (`+0xf4`, `-0x3cc`, `+0x378`), are noise, mostly data
symbols matching by coincidence. The script now reports strong and weak runs
separately and no longer quotes an overall coverage figure.

The exact insertion points are also not established — only that they exist and
roughly where. The first `codeseg` region (`-0x7b0`, 6 symbols) is likely an
artifact of assuming Rev A's `codeseg` ROM base for v1.0, which the rest of this
document shows is wrong.

## Consequence for the plan

Porting the corpus by address arithmetic is off the table. The revised phase 01
approach:

1. Disassemble **our** dump and recover function boundaries from it, rather than
   inheriting Rev A's. This was always the plan for boundaries; what changes is
   that we can no longer shortcut it.
2. Attach Rev A *names* by aligning the two function sequences — same order,
   matching bodies — rather than by address. Function order is preserved between
   revisions, which makes this a sequence alignment problem and a tractable one.
3. Treat every ported name as provisional until its function body matches.

This is more work than a constant offset would have been and much less than
starting from nothing. The regions above give the alignment a strong prior:
anywhere the expected delta holds, a match is nearly certain.

The v1.0 `codeseg` ROM base is **not** `0xA95D0`; that value is Rev A's and must
be re-derived from our own disassembly before any splat config uses it.

---

# Addendum: switched to Rev A

A Rev A dump became available, and it is byte-identical to the one LLONSIT's
decomp pins:

```
sha1 508dfc2d4caa42b6f6de5263d0aed5e44ac7966a
```

Everything above about v1.0 is therefore superseded as a build target. It is
kept because it remains the only measurement of how the two revisions relate,
which matters if v1.0 support is ever wanted, and because it is the evidence
that switching was right: porting the corpus to v1.0 would have meant sequence
alignment across piecewise offsets, for no benefit now that the revision the
corpus was written for is in hand.

The project is re-pinned to Rev A (revision 1, CRC `0x492F4B61 0x04E5146A`).

## What now works

| | |
|---|---|
| splat | 0.37.1 vendored by the decomp, on spimdisasm 1.42.4 |
| disassembly | 1,537 `.s` files, **1,346 functions** |
| asm-only config | `recomp/wr64.us.rev1.asm.yaml`, 157 `c` subsegments rewritten to `asm` |
| objects | 380 built, 0 failures |
| link | succeeds |
| ELF | 10.3 MB, **1,346 FUNC** symbols, 7,205 OBJECT symbols, 83 sections |
| relocations | **22 `.rel.*` sections** — `.rel.main_segment`, `.rel.codeseg`, one per overlay |

The relocation sections are the part that matters most for what comes next:
they are exactly what N64Recomp's `relocatable_sections_path` consumes, and
their absence is what forced both prior Wave Race 64 attempts to disable
overlay relocation.

## The gate is NOT met

The phase 01 gate is an ELF whose code is byte-identical to the ROM. It is not.

Comparing each code section against its known ROM offset:

| section | size | differing bytes |
|---|---:|---:|
| `.entry` | `0x50` | 4 (5.0%) |
| `.main_segment` | `0xa5500` | 59,611 (8.8%) |
| `.codeseg` | `0x4c600` | 6,654 (2.1%) |
| `.segment_1B1FB0` | `0x1f00` | 38 (0.48%) |
| `.ovl_i0` | `0x16a0` | 93 (1.6%) |
| `.ovl_i1` | `0x3d80` | 550 (3.5%) |

So 91-99% correct, which means the segment map and section placement are right
and something narrower is wrong.

### What the difference is not

The first two samples inspected differed by exactly `0x3080` in their immediate
fields, suggesting a single constant offset in synthesized symbol values. Across
all 24,287 differing instruction words that hypothesis fails:

- `+0x3080` accounts for only 13.4% of deltas; `-0xcf80` for 6.5%; the rest
  scatter.
- **24.7% of differing words are opcode `0x00`** — R-type instructions, which
  have no immediate field at all.

A wrong relocation value cannot change an R-type instruction. So this is not
purely a symbol-address problem, and the constant-offset lead is dead.

### Prime suspect

The link needed 1,180 symbols that no assembled object defines: 1,126 were
synthesized from the addresses encoded in their own names, and 54 looked up in
the decomp's corpus. Those synthesized definitions are the least trustworthy
thing in the build, and they are worth eliminating before anything else --
preferably by having splat emit real definitions for the data and rodata that
currently arrives as opaque `bin` blobs, rather than by inventing addresses.

The R-type differences need a separate explanation and should be chased first,
by dumping one small differing region side by side rather than in aggregate.
`.segment_1B1FB0` at 0.48% and `.entry` at 4 bytes are the right places to look:
small enough to read by hand.

---

# Addendum 2: the 4 bytes in `.entry`, explained

Starting from the smallest failing case turned out to explain the whole thing.
`.entry` is 0x50 bytes and only two words differ:

| vram | ELF | ROM | resolves to (ELF / ROM) |
|---|---|---|---|
| `0x80046808` | `2508BD50` | `2508EDD0` | `0x800EBD50` / `0x800EEDD0` |
| `0x80046834` | `27BDEB60` | `27BD1BE0` | `0x8014EB60` / `0x80151BE0` |

Both are the `addiu` half of a `lui`/`addiu` address pair, and both are low by
exactly `0x3080`. The second is the boot code setting the stack pointer, so the
port would have started with a stack 12,416 bytes below where the game expects
it -- a fault that would have surfaced during phase 04 as an inexplicable early
crash, far from its cause.

## Root cause

`D_80151BE0` -- a symbol whose name states its own address -- is placed by our
link at `0x8014EB60`. `main_segment_BSS_START` lands at `0x800EBD50` where the
ROM requires `0x800EEDD0`.

Tracking every address-encoding symbol the linker placed (2,551 of them) shows
the error is not one missing chunk but an accumulation:

| declared | placed | drift |
|---|---|---|
| `0x80038220` | `0x80038220` | 0 |
| `0x800D4644` | `0x800D4634` | `-0x10` |
| `0x800DAB10` | `0x800DA910` | `-0x200` |
| `0x800E9870` | `0x800E8D90` | `-0xAE0` |
| `0x800EB4C0` | `0x800E9050` | `-0x2470` |
| `0x800EC830` | `0x800E97B0` | `-0x3080` |

675 symbols (26.5%) are placed exactly -- the code. The drift is confined to
data and rodata, and grows monotonically.

The first slip is exact and readable. `Seed` sits at `0x800D4630` with size 4,
so it ends at `0x800D4634`. The next subsegment file, `8EE40.data.s`, begins at
ROM `0x8EE40`, i.e. vram `0x800D4640`. The 0x10 bytes between are real bytes in
the cartridge that no symbol covers, so splat never emits them, the linker
concatenates the next object immediately, and everything downstream shifts.

**splat emits each data subsegment only as far as its last symbol.** Trailing
bytes not covered by a symbol are dropped. Nothing realigns the next object, so
each shortfall is permanent and they sum to `0x3080` by the end of
`main_segment`.

This also disposes of the "R-type instructions differ" puzzle from addendum 1.
`.main_segment` holds text *and* data in one section, so once the data drifts,
a byte-for-byte comparison at a fixed ROM offset is misaligned from that point
on and every later word looks wrong. There was only ever one bug.

## Two fixes tried and rejected

Both were measured, not assumed, and neither changed the output by one byte:

- `migrate_rodata_to_functions: False` -- inert, because it only affects `c`
  subsegments and ours are all `asm`.
- `subalign: 16` -- splat emits `ALIGN` at section boundaries, not between
  objects, which is exactly where the padding is missing.

## The fix to make next

Pad each generated data object out to its true length. The length is knowable
without guessing: a subsegment runs from its own ROM start to the next
subsegment's ROM start, and both are in the config. Emitting `.space` for the
difference at the end of each generated `.s` restores the dropped bytes
exactly, rather than inferring an alignment the original build may not have used.

Verify with the same measurement: `675/2551` symbols placed exactly should
become `2551/2551`, and `.entry` should match the cartridge in all 0x50 bytes.
