"""Phase 01: recover the piecewise Rev A -> v1.0 address map.

probe_delta.py found a dominant shift of -0x2A0 for codeseg at 55% coverage,
with a second peak at -0x270. That is the signature of code inserted partway
through the segment in Rev A: functions before the insertion point shift by one
amount, functions after it by another.

This script recovers the whole map. For every Rev A function address it
computes the set of deltas that would place it on a function boundary in our
dump, then sweeps the addresses in order and reports the regions over which a
single delta holds.

The output is the thing phase 01 actually needs: either a small set of regions,
in which case the Rev A symbol corpus ports across mechanically, or a mess, in
which case boundaries have to come from our own disassembly.

Run from the repository root:
    python tools/probe_piecewise.py rom.z64
"""

import re
import struct
import sys
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
REV_A_SYMS = REPO / "reference" / "wr64-decomp" / "linker_scripts" / "us" / "rev1"

JR_RA = 0x03E00008
NOP = 0x00000000

SEGMENTS = [
    ("main_segment", 0x1050, 0x80046850, 0xA95D0),
    ("codeseg", 0xA95D0, 0x801DAFA0, 0xF6090),
]

WINDOW_LO, WINDOW_HI = -0x800, 0x400
LOOKAHEAD = 8
MIN_RUN = 5


def load_symbols():
    syms = {}
    for path in sorted(REV_A_SYMS.glob("*.txt")):
        for line in path.read_text(errors="replace").splitlines():
            m = re.match(r"\s*([A-Za-z_][\w]*)\s*=\s*(0x[0-9A-Fa-f]+)\s*;", line)
            if m:
                syms[m.group(1)] = int(m.group(2), 0)
    return syms


def word_at(data, off):
    if off < 0 or off + 4 > len(data):
        return None
    return struct.unpack(">I", data[off:off + 4])[0]


def is_boundary(data, off):
    prev2 = word_at(data, off - 8)
    prev1 = word_at(data, off - 4)
    here = word_at(data, off)
    if here is None:
        return False
    if prev2 == JR_RA:
        return True
    if prev1 == NOP and prev2 == NOP:
        return True
    if (here >> 26) == 0x09 and ((here >> 21) & 0x1F) == 29 \
            and ((here >> 16) & 0x1F) == 29 and (here & 0x8000):
        return True
    return False


def candidate_deltas(data, rom_off):
    return {d for d in range(WINDOW_LO, WINDOW_HI + 1, 4)
            if is_boundary(data, rom_off + d)}


def sweep(data, entries, rom_base, vram_base, label):
    """entries: sorted list of (name, vram). Report piecewise-constant deltas."""
    print("=" * 72)
    print(f"{label}")
    print("=" * 72)

    cands = []
    for name, vram in entries:
        off = rom_base + (vram - vram_base)
        cands.append((name, vram, candidate_deltas(data, off)))

    unexplained = [c for c in cands if not c[2]]
    usable = [c for c in cands if c[2]]
    print(f"  symbols considered      : {len(cands)}")
    print(f"  with no boundary at any delta in [{WINDOW_LO:#x}, {WINDOW_HI:#x}]: "
          f"{len(unexplained)}  (likely data, not functions)")
    print(f"  usable for mapping      : {len(usable)}")
    if not usable:
        print()
        return []

    regions = []
    cur = None
    start_vram = None
    covered = 0

    for i, (name, vram, ds) in enumerate(usable):
        if cur is not None and cur in ds:
            covered += 1
            continue
        # Choose the delta that best explains this symbol and the ones after it.
        window = usable[i:i + LOOKAHEAD]
        tally = Counter()
        for _, _, d2 in window:
            for d in d2:
                tally[d] += 1
        best = None
        for d, n in sorted(tally.items(), key=lambda kv: (-kv[1], abs(kv[0]))):
            if d in ds:
                best = d
                break
        if best is None:
            best = min(ds, key=abs)
        if cur is not None:
            regions.append((start_vram, usable[i - 1][1], cur, covered))
        cur = best
        start_vram = vram
        covered = 1

    if cur is not None:
        regions.append((start_vram, usable[-1][1], cur, covered))

    # A greedy sweep over a wide delta window can "explain" everything, so the
    # overall coverage number is meaningless. Only long runs are evidence: a
    # single delta holding across many consecutive symbols is not something
    # chance produces. Short runs are reported separately as noise.
    strong = [r for r in regions if r[3] >= MIN_RUN]
    weak = [r for r in regions if r[3] < MIN_RUN]

    print()
    print(f"  high-confidence regions (>= {MIN_RUN} consecutive symbols at one delta):")
    print(f"  {'vram range':<25} {'delta':>10} {'symbols':>9}")
    for lo, hi, d, n in strong:
        print(f"  0x{lo:08X} - 0x{hi:08X} {d:>+#10x} {n:>9}")

    strong_n = sum(n for *_, n in strong)
    weak_n = sum(n for *_, n in weak)
    print()
    print(f"  {len(strong)} strong region(s) cover {strong_n}/{len(usable)} symbols "
          f"({100.0 * strong_n / len(usable):.1f}%)")
    print(f"  {len(weak)} short run(s) cover the remaining {weak_n} "
          f"-- treat these as unexplained, not as regions")

    if strong:
        deltas = [d for *_, d, _ in [(r[0], r[1], r[2], r[3]) for r in strong]]
        print(f"  deltas in address order: "
              f"{', '.join(format(d, '+#x') for d in deltas)}")
        if all(b <= a for a, b in zip(deltas, deltas[1:])):
            print("  -> monotonically decreasing: consistent with Rev A having")
            print("     INSERTED code at several points, each insertion adding to a")
            print("     cumulative offset. Not a single constant shift.")
    print()
    return strong


def main():
    rom_path = Path(sys.argv[1] if len(sys.argv) > 1 else "rom.z64")
    data = rom_path.read_bytes()
    syms = load_symbols()

    print(f"probing {rom_path}  ({len(data)} bytes)")
    print(f"Rev A symbols: {len(syms)}\n")

    for label, rom, vram, rom_end in SEGMENTS:
        span = rom_end - rom
        entries = sorted(((n, a) for n, a in syms.items() if vram <= a < vram + span),
                         key=lambda kv: kv[1])
        sweep(data, entries, rom, vram, label)


if __name__ == "__main__":
    main()
