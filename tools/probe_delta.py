"""Phase 01: find the ROM offset delta between Rev A and our v1.0 dump.

probe_layout.py established that main_segment addresses transfer from Rev A to
v1.0 but codeseg addresses do not. The question this answers is whether codeseg
is merely *shifted* -- in which case the whole Rev A symbol corpus still ports
across by adding a constant -- or genuinely rearranged, which would mean
recovering boundaries from our own dump.

Method: for each candidate delta, relocate every Rev A `func_<hex>` address by
that delta and count how many land on a defensible function boundary. A real
shift shows up as a sharp spike at one delta. Noise has no spike.

Run from the repository root:
    python tools/probe_delta.py rom.z64
"""

import re
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
REV_A_SYMS = REPO / "reference" / "wr64-decomp" / "linker_scripts" / "us" / "rev1"

JR_RA = 0x03E00008
NOP = 0x00000000

# (name, rom start, vram start, rom end) exactly as the Rev A config declares.
SEGMENTS = [
    ("main_segment", 0x1050, 0x80046850, 0xA95D0),
    ("codeseg", 0xA95D0, 0x801DAFA0, 0xF6090),
]

SCAN_RADIUS = 0x4000  # +/- 16 KiB, stepped by one instruction


def load_func_symbols():
    funcs = {}
    for path in sorted(REV_A_SYMS.glob("*.txt")):
        for line in path.read_text(errors="replace").splitlines():
            m = re.match(r"\s*([A-Za-z_][\w]*)\s*=\s*(0x[0-9A-Fa-f]+)\s*;", line)
            if not m:
                continue
            name, addr = m.group(1), int(m.group(2), 0)
            fm = re.fullmatch(r"func_([0-9A-Fa-f]{8})", name)
            if fm and int(fm.group(1), 16) == addr:
                funcs[name] = addr
    return funcs


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


def scan(data, addrs, rom_base, vram_base, label):
    """Score every candidate delta for this set of addresses."""
    print("=" * 70)
    print(f"{label}: {len(addrs)} function addresses")
    print("=" * 70)
    if not addrs:
        print("  (no function symbols in this segment)\n")
        return

    scores = []
    for delta in range(-SCAN_RADIUS, SCAN_RADIUS + 1, 4):
        hits = 0
        for vram in addrs:
            off = rom_base + (vram - vram_base) + delta
            if is_boundary(data, off):
                hits += 1
        scores.append((hits, delta))

    scores.sort(key=lambda x: (-x[0], abs(x[1])))
    best_hits, best_delta = scores[0]

    print(f"  best delta : {best_delta:+#x} ({best_delta:+d} bytes)")
    print(f"  hit rate   : {best_hits}/{len(addrs)} = {100.0 * best_hits / len(addrs):.1f}%")

    baseline = sorted(h for h, _ in scores)
    median = baseline[len(baseline) // 2]
    print(f"  background : median {median}/{len(addrs)} = "
          f"{100.0 * median / len(addrs):.1f}% across all {len(scores)} candidate deltas")

    print("\n  top candidates:")
    for hits, delta in scores[:6]:
        bar = "#" * int(60.0 * hits / len(addrs))
        print(f"    {delta:+#8x}  {hits:>4}/{len(addrs)}  {bar}")

    if best_hits >= max(3 * median, 0.5 * len(addrs)):
        print(f"\n  -> A clear shift of {best_delta:+#x}. The Rev A corpus ports across")
        print("     this segment by adding that constant.")
    else:
        print("\n  -> No spike stands out above the background. This segment is not a")
        print("     simple shift; boundaries must be recovered from our own dump.")
    print()


def main():
    rom_path = Path(sys.argv[1] if len(sys.argv) > 1 else "rom.z64")
    data = rom_path.read_bytes()
    funcs = load_func_symbols()
    print(f"probing {rom_path}  ({len(data)} bytes)")
    print(f"Rev A func_<hex> symbols: {len(funcs)}\n")

    for name, rom, vram, rom_end in SEGMENTS:
        span = rom_end - rom
        addrs = [a for a in funcs.values() if vram <= a < vram + span]
        scan(data, addrs, rom, vram, name)


if __name__ == "__main__":
    main()
