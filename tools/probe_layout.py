"""Phase 01 measurement: does the Rev A segment map fit our v1.0 dump?

The revision choice rests on the assumption that v1.0 and Rev A share a link
layout. This probe tests that assumption against the ROM itself.

Method. A naive test -- "do Rev A symbols land on a function prologue" -- is
useless, because most of the Rev A corpus names data, and leaf functions have
no `addiu $sp` prologue anyway. So we test two things that are actually sound:

  1. `func_<hex>` symbols encode their own address in their name, so every one
     of them is a function by construction. That is a clean denominator.
  2. A MIPS function boundary is recognisable from the code *before* it: the
     previous function ends `jr $ra` followed by its delay slot. Checking the
     two words before a claimed function start is far stronger evidence than
     inspecting the start itself.

Run from the repository root:
    python tools/probe_layout.py rom.z64
"""

import re
import struct
import sys
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
REV_A_YAML = REPO / "reference" / "wr64-decomp" / "waverace64.us.rev1.yaml"
REV_A_SYMS = REPO / "reference" / "wr64-decomp" / "linker_scripts" / "us" / "rev1"

JR_RA = 0x03E00008
NOP = 0x00000000

# Code segments as the Rev A config declares them: (rom start, vram, rom end).
SEGMENTS = [
    ("main_segment", 0x1050, 0x80046850, 0xA95D0),
    ("codeseg", 0xA95D0, 0x801DAFA0, 0xF6090),
]


def load_rev_a_segment_starts():
    starts = []
    name = None
    for line in REV_A_YAML.read_text().splitlines():
        m = re.match(r"\s*-\s*name:\s*(\S+)", line)
        if m:
            name = m.group(1)
            continue
        m = re.match(r"\s*start:\s*(0x[0-9A-Fa-f]+|\d+)", line)
        if m and name:
            starts.append((name, int(m.group(1), 0)))
            name = None
    return starts


def load_rev_a_symbols():
    syms = {}
    for path in sorted(REV_A_SYMS.glob("*.txt")):
        for line in path.read_text(errors="replace").splitlines():
            m = re.match(r"\s*([A-Za-z_][\w]*)\s*=\s*(0x[0-9A-Fa-f]+)\s*;", line)
            if m:
                syms[m.group(1)] = int(m.group(2), 0)
    return syms


def vram_to_rom(vram):
    for _, rom, vr, rom_end in SEGMENTS:
        if vr <= vram < vr + (rom_end - rom):
            return rom + (vram - vr)
    return None


def word_at(data, off):
    if off < 0 or off + 4 > len(data):
        return None
    return struct.unpack(">I", data[off:off + 4])[0]


def boundary_evidence(data, off):
    """Classify how strongly `off` looks like the start of a function."""
    prev2 = word_at(data, off - 8)
    prev1 = word_at(data, off - 4)
    here = word_at(data, off)
    if here is None:
        return "out-of-range"

    # A function that ends `jr $ra` + delay slot immediately precedes this one.
    if prev2 == JR_RA:
        return "after-jr-ra"
    # Padding between functions is common: jr $ra, delay slot, then nops.
    if prev1 == NOP and prev2 == NOP:
        return "after-padding"
    # The word itself opens a stack frame.
    if (here >> 26) == 0x09 and ((here >> 21) & 0x1F) == 29 \
            and ((here >> 16) & 0x1F) == 29 and (here & 0x8000):
        return "prologue"
    return "no-evidence"


def main():
    rom_path = Path(sys.argv[1] if len(sys.argv) > 1 else "rom.z64")
    data = rom_path.read_bytes()
    print(f"probing {rom_path}  ({len(data)} bytes)\n")

    # ---------- 1. Data-region layout, via MIO0 magic ----------
    ours = [m.start() for m in re.finditer(b"MIO0", data)]
    print("=" * 70)
    print("1. Data region: MIO0 block positions")
    print("=" * 70)
    print(f"MIO0 blocks in our v1.0 dump : {len(ours)}")
    print(f"first at 0x{ours[0]:X}, last at 0x{ours[-1]:X}")
    rev_a_first = 0x1AE660  # Rev A's MIO0_chunk segment start
    delta = rev_a_first - ours[0]
    print(f"Rev A declares its first MIO0 segment at 0x{rev_a_first:X}")
    print(f"  -> our v1.0 data region sits 0x{delta:X} ({delta}) bytes EARLIER")
    print("     i.e. Rev A is the larger image, as the later revision should be.")

    # ---------- 2. Code region, via func_<hex> symbols ----------
    syms = load_rev_a_symbols()
    func_syms = {}
    for name, addr in syms.items():
        m = re.fullmatch(r"func_([0-9A-Fa-f]{8})", name)
        if m and int(m.group(1), 16) == addr:
            func_syms[name] = addr

    print()
    print("=" * 70)
    print("2. Code region: are Rev A function addresses valid in v1.0?")
    print("=" * 70)
    print(f"Rev A symbols total                    : {len(syms)}")
    print(f"of which are func_<hex> (functions)    : {len(func_syms)}")

    tally = Counter()
    per_segment = {}
    misses = []
    for name, vram in sorted(func_syms.items(), key=lambda kv: kv[1]):
        off = vram_to_rom(vram)
        if off is None:
            tally["outside-mapped-code"] += 1
            continue
        verdict = boundary_evidence(data, off)
        tally[verdict] += 1
        seg = next((s for s, r, v, e in SEGMENTS if v <= vram < v + (e - r)), "?")
        per_segment.setdefault(seg, Counter())[verdict] += 1
        if verdict == "no-evidence":
            misses.append((name, vram, off))

    inside = sum(v for k, v in tally.items() if k != "outside-mapped-code")
    good = tally["after-jr-ra"] + tally["after-padding"] + tally["prologue"]

    print(f"inside a mapped code segment          : {inside}")
    print()
    for verdict in ("after-jr-ra", "after-padding", "prologue", "no-evidence"):
        if tally[verdict]:
            pct = 100.0 * tally[verdict] / inside if inside else 0
            print(f"  {verdict:<16} {tally[verdict]:>5}   {pct:5.1f}%")
    if tally["outside-mapped-code"]:
        print(f"  {'outside code':<16} {tally['outside-mapped-code']:>5}")

    if inside:
        print()
        print(f"  VERDICT: {good}/{inside} = {100.0 * good / inside:.1f}% of Rev A function")
        print("  addresses land on a defensible function boundary in our v1.0 dump.")

    print()
    print("per code segment:")
    for seg, counts in per_segment.items():
        tot = sum(counts.values())
        ok = tot - counts["no-evidence"]
        print(f"  {seg:<14} {ok}/{tot} = {100.0 * ok / tot:.1f}%")

    if misses:
        print(f"\naddresses with no boundary evidence ({len(misses)}):")
        for name, vram, off in misses[:15]:
            w = word_at(data, off)
            print(f"  {name:<20} vram 0x{vram:08X}  rom 0x{off:06X}  word 0x{w:08X}")
        if len(misses) > 15:
            print(f"  ... and {len(misses) - 15} more")


if __name__ == "__main__":
    main()
