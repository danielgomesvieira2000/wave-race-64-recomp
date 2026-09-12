"""Decode the game's wave field, and check the decode against the game itself.

Wave Race 64's water is a **world-fixed, wrapping, triangular lattice** of
64-unit cells. `func_8004F3D4` in the decompilation gives the indexing in C, and
this is that arithmetic rewritten in Python:

    u   = int(x + z * (1/sqrt(3))) % 24576
    v   = int(    z * (2/sqrt(3))) % 24576
    row = ((v >> 6) + ((u >> 6) & ~0x7F) + 0x600) % 384
    col =  (u >> 6) & 0x7F

    D_80162420[row * 128 + col] = { s16 height, s16 age }

384 rows x 128 columns x 4 bytes is 192 KiB, which puts the array's end at
exactly 0x80192420 -- the address func_80050204's split-screen test reads, and
just below gWaterLevel at 0x80192458.

**The check is the point.** Reading the code gives a model; comparing it against
`func_8004D30C(x, z)` -- the game's own height query, probed on a grid by
src/waterfield.cpp -- is what turns the model into something geometry can be
built from. A decode that reproduces the game's answers is trustworthy; one that
does not localises the error, and the two likeliest places are the row folding
(`(u >> 6) & ~0x7F`) and the `0x600` bias.

    python tools/decode_water_field.py build/field
    python tools/decode_water_field.py build/field --image build/field.png

Reads `<prefix>-field.bin`, `<prefix>-probe.csv` and `<prefix>-meta.txt`, which
`WR64_WATER_FIELD=build/field` produces. See docs/GAME-INTERNALS.md.
"""

import argparse
import math
import struct
import sys
from pathlib import Path

ROWS, COLS, CELL = 384, 128, 64
WRAP = 24576                      # 384 * 64
ONE_OVER_SQRT3 = 0.57735026
TWO_OVER_SQRT3 = 1.1547005

# The s16 is in 1/256 world units -- func_8004F3D4 writes `arg2 * 256` -- but
# what the readers use is `s16 >> 8`, an arithmetic shift: a *whole* world unit,
# floored, with the fraction discarded. func_8004D30C does it at 0x8004D41C
# and the mesh builder does the same.
#
# Treating it as h/256.0 instead is wrong by the discarded fraction, which
# averages half a unit -- and measured against the game's own query that is
# exactly what it cost: a median error of 0.4950. Getting this wrong looks like
# a plausible-but-imperfect decode rather than a broken one, which is why it
# survived several rounds of checking.
HEIGHT_SHIFT = 8


def c_trunc(value: float) -> int:
    """A C cast from float to s32: truncates toward zero, not toward -inf."""
    return int(value)          # Python's int() on a float already truncates


def c_mod(a: int, b: int) -> int:
    """C's %, which keeps the sign of the dividend. Python's floors instead."""
    r = abs(a) % abs(b)
    return r if a >= 0 else -r


def entry_index(cu: int, cv: int):
    """(row, col) for a lattice cell, as func_8004F3D4 addresses one.

    The array is 128 columns wide in u, and the row absorbs everything above
    that: row = cv + (cu & ~0x7F). So a sheared 2D space is packed into 384x128
    by folding u's high bits into the row, which is why the row depends on u at
    all. The +0x600 bias is 4 * 384 and keeps the sum positive before the
    modulo; without it a negative coordinate would index behind the array. That
    is also why C's % has to be emulated rather than Python's used: they only
    agree once the bias has done its job.
    """
    row = c_mod(cv + (cu & ~0x7F) + 0x600, ROWS)
    col = cu & 0x7F
    return row, col


def axial(x: float, z: float):
    """World XZ to the lattice's axial coordinates, in cells, with fractions."""
    u = c_mod(c_trunc(x + z * ONE_OVER_SQRT3), WRAP)
    v = c_mod(c_trunc(z * TWO_OVER_SQRT3), WRAP)
    return (u >> 6, v >> 6, (u & 63) / 64.0, (v & 63) / 64.0)


def height_at(heights, x: float, z: float) -> float:
    """The surface height at a world XZ: a plane over the containing triangle.

    A single cell lookup is not the model. func_8004D30C builds a triangular
    plane and evaluates the point against it, and func_8004F3D4 shows which
    diagonal splits the rhombus: it compares the two fractional parts, so the
    split runs along fu == fv, and its two candidate cells -- (cu, cv+1) and
    (cu+1, cv) -- are the off-diagonal corner of each half.

        fu <  fv   corners (cu,cv), (cu,cv+1), (cu+1,cv+1)
        fu >= fv   corners (cu,cv), (cu+1,cv), (cu+1,cv+1)

    Barycentric weights fall straight out of the local coordinates, which is
    what the two branches below are.
    """
    cu, cv, fu, fv = axial(x, z)

    def h(du: int, dv: int) -> float:
        row, col = entry_index(cu + du, cv + dv)
        return heights[row][col]

    if fu < fv:
        return (1.0 - fv) * h(0, 0) + (fv - fu) * h(0, 1) + fu * h(1, 1)
    return (1.0 - fu) * h(0, 0) + (fu - fv) * h(1, 0) + fv * h(1, 1)


def load_field(path: Path):
    """The field as two ROWS x COLS lists: heights in world units, and ages.

    RDRAM is stored as host-endian 32-bit words, so within each 4-byte entry the
    two halfwords are swapped relative to the cartridge's big-endian layout --
    the same reason src/water.cpp XORs a halfword address with 2. Height is the
    halfword at cartridge offset 0, which lands in the *upper* two bytes here.
    """
    raw = path.read_bytes()
    expected = ROWS * COLS * 4
    if len(raw) != expected:
        sys.exit(f"{path}: expected {expected} bytes, got {len(raw)}")

    heights = [[0] * COLS for _ in range(ROWS)]
    ages = [[0] * COLS for _ in range(ROWS)]
    for i in range(ROWS * COLS):
        age, height = struct.unpack_from("<hh", raw, i * 4)
        # Python's >> on a negative int floors, which is what MIPS sra does.
        heights[i // COLS][i % COLS] = height >> HEIGHT_SHIFT
        ages[i // COLS][i % COLS] = age
    return heights, ages


def read_meta(path: Path):
    meta = {}
    if path.is_file():
        for line in path.read_text(encoding="utf-8").splitlines():
            parts = line.split()
            if len(parts) >= 2:
                meta[parts[0]] = parts[1]
    return meta


def describe_field(heights, ages):
    flat = [h for row in heights for h in row]
    nonzero = [h for h in flat if h != 0.0]
    aged = [a for row in ages for a in row if a != 0]
    print("--- the field ---")
    print(f"  cells            {len(flat)}  ({ROWS} rows x {COLS} cols, {CELL}-unit cells)")
    print(f"  disturbed        {len(nonzero)}  ({100.0 * len(nonzero) / len(flat):.1f}%)")
    if nonzero:
        print(f"  height range     {min(nonzero):+d} .. {max(nonzero):+d} world units")
        print(f"  mean |height|    {sum(abs(h) for h in nonzero) / len(nonzero):.2f}")
    print(f"  nonzero age      {len(aged)}")
    if aged:
        print(f"  age range        {min(aged)} .. {max(aged)}")
    print(f"  world period     u {WRAP} units, v {WRAP} units")
    print(f"  column span      {COLS * CELL} units of u before the row folds")


def check_probes(path: Path, heights, water_level: float, quantum: float):
    """Compare the decode against the game's own height query."""
    if not path.is_file():
        print(f"\n--- probes ---\n  {path} missing; decode unverified")
        return False

    per_grid = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#") or line.startswith("grid,"):
            continue
        grid, xs, zs, hs = line.split(",")
        x, z, game = float(xs), float(zs), float(hs)
        ours = height_at(heights, x, z) + water_level
        per_grid.setdefault(grid, []).append(abs(ours - game))

    print("\n--- decode vs the game's own func_8004D30C ---")
    print(f"  heights are whole world units after >>8; tolerance {quantum:.4f}\n")
    print(f"  {'grid':<8} {'probes':>7} {'within':>8} {'median':>9} {'p95':>9} {'max':>9}")
    ok = True
    for grid, errors in sorted(per_grid.items()):
        errors.sort()
        n = len(errors)
        within = sum(1 for e in errors if e <= quantum)
        median = errors[n // 2]
        p95 = errors[min(n - 1, int(n * 0.95))]
        print(f"  {grid:<8} {n:>7} {100.0 * within / n:>7.1f}% "
              f"{median:>9.4f} {p95:>9.4f} {errors[-1]:>9.4f}")
        # A plane over the triangle should agree everywhere, not only on the
        # corners, so every grid counts now. The lattice grid is the strictest
        # of the three anyway: probing exactly on a cell boundary is where a
        # truncating index is most easily off by one.
        if within < 0.99 * n:
            ok = False

    print()
    if ok:
        print("  VERDICT: the model reproduces the game everywhere probed. The field,")
        print("           its indexing and its interpolation are all accounted for.")
    else:
        print("  VERDICT: not yet exact. Read the per-grid rows: a lattice row that is")
        print("           worse than the interior rows points at the index (boundary")
        print("           truncation, the row folding, the 0x600 bias); interior rows")
        print("           worse than the corners point at the interpolation instead.")
    return ok


def write_image(path: Path, heights):
    """A greyscale PGM of the field. No dependencies; any viewer opens it."""
    flat = [h for row in heights for h in row]
    lo, hi = min(flat), max(flat)
    span = (hi - lo) or 1.0
    with path.open("wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (COLS, ROWS))
        f.write(bytes(int(255 * (h - lo) / span) for row in heights for h in row))
    print(f"\nwrote {path}  ({COLS}x{ROWS}, heights {lo:+.3f}..{hi:+.3f})")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("prefix", help="the WR64_WATER_FIELD prefix that was dumped")
    parser.add_argument("--image", type=Path, help="write a PGM of the field here")
    args = parser.parse_args()

    base = Path(args.prefix)
    heights, ages = load_field(Path(str(base) + "-field.bin"))
    meta = read_meta(Path(str(base) + "-meta.txt"))

    water_level = float(meta.get("water_level", 0))
    print(f"--- meta ---\n  course {meta.get('course','?')}, tick {meta.get('tick','?')}, "
          f"gWaterLevel {water_level:.0f}")

    describe_field(heights, ages)
    # The readers floor to whole units, so the tolerance is rounding, not 1/256.
    ok = check_probes(Path(str(base) + "-probe.csv"), heights, water_level, 0.01)

    if args.image:
        write_image(args.image, heights)

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
