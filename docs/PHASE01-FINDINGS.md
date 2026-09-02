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
