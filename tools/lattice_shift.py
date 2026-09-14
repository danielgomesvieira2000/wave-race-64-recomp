"""Does the water lattice carry its heights, or resample a world-fixed field?

Two accounts of the same mesh have been in circulation, and the water renderer
this port ships depends on which is true.

  **Carried.** The lattice moves as a rigid body and the wave pattern moves with
  it, so vertex *i* means the same piece of water from frame to frame and
  pairing by index is sound. This is what `docs/GAME-INTERNALS.md` concluded
  from per-block summaries over 2,172 frames.

  **Resampled.** The heights come from a world-fixed field -- which
  `D_80162420` demonstrably is -- so when the lattice moves, vertex *i* lands on
  a different piece of water and takes a different height. Pairing by index then
  blends unrelated positions, which is what the fork concluded and why its
  renderer resamples the previous surface at each current world XZ.

They leave different marks in the same data, and this looks for them. Across a
frame where the lattice carried by `d` vertices' worth of spacing:

    carried     y_i(t+1) == y_i(t)                 -- no shift
    resampled   y_i(t+1) == y_(i+d)(t)             -- shifted along the index

So: correlate each frame's heights against the previous frame's at every shift,
and report which shift fits best. A best shift of zero says carried; a best
shift that tracks the lattice's movement says resampled.

    WR64_LATTICE_VERTS=verts.csv WR64_LATTICE_VERT_FRAMES=12 <the port>
    python tools/lattice_shift.py verts.csv
"""

import argparse
import sys
from collections import defaultdict
from pathlib import Path


def load(path: Path, courses: set):
    """frame -> {(block, index): (x, y, z)}.

    The lattice is two-dimensional -- a block is a row of it, the index is the
    position along that row -- so the shift has to be searched in both. A
    one-dimensional search over the flattened order finds a column shift and is
    blind to a row shift, which is how a carry of one column *and* one row
    reads as no shift at all.
    """
    frames = defaultdict(dict)
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#") or line.startswith("frame,"):
            continue
        parts = line.split(",")
        # 9 columns since the texture coordinates were added, 7 since the course
        # was added; 6 is the oldest layout.
        if len(parts) in (7, 9):
            frame, course, block, index, x, y, z = (int(v) for v in parts[:7])
        elif len(parts) == 6:
            frame, block, index, x, y, z = (int(v) for v in parts)
            course = -1
        else:
            continue          # the run is killed mid-write; the last line is short
        frames[frame][(block, index)] = (x, y, z)
        courses.add(course)
    return frames


def best_shift(prev, cur, limit: int):
    """The index shift at which this frame's heights best match the last one's.

    Scored by mean absolute difference over the overlap, which is robust to the
    ends of the array falling off as the lattice moves.
    """
    best = (float("inf"), 0, 0)
    for drow in range(-limit, limit + 1):
        for dcol in range(-limit, limit + 1):
            total, n = 0, 0
            for (block, index), (_x, y, _z) in cur.items():
                other = prev.get((block + drow, index + dcol))
                if other is not None:
                    total += abs(y - other[1])
                    n += 1
            if n >= len(cur) // 3:
                mean = total / n
                if mean < best[0]:
                    best = (mean, drow, dcol)
    return best


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("csv", type=Path)
    parser.add_argument("--limit", type=int, default=3,
                        help="largest row/column shift to consider (default 3)")
    args = parser.parse_args()

    courses = set()
    frames = load(args.csv, courses)
    if len(frames) < 2:
        sys.exit(f"{args.csv}: need at least two frames, found {len(frames)}")

    order = sorted(frames)
    named = ", ".join(str(c) for c in sorted(courses) if c >= 0) or "unlabelled"
    print(f"course {named}")
    print(f"{len(order)} frames, {len(frames[order[0]])} vertices in the first\n")
    print(f"  {'frame':>6} {'verts':>6} {'moved x':>8} {'moved z':>8} "
          f"{'best row,col':>13} {'err@best':>9} {'err@0':>9}")

    zero_wins = 0
    shifted_wins = 0
    best_errors = []
    clean_frames = []
    for a, b in zip(order, order[1:]):
        prev, cur = frames[a], frames[b]
        if not prev or not cur:
            continue
        first = min(cur)
        dx = cur[first][0] - prev[first][0] if first in prev else 0
        dz = cur[first][2] - prev[first][2] if first in prev else 0
        err, drow, dcol = best_shift(prev, cur, args.limit)
        best_errors.append(err)
        err0, _, _ = best_shift(prev, cur, 0)
        clean_frames.append((dx, dz, err0, err))
        moved = dx != 0 or dz != 0
        if moved:
            if (drow, dcol) == (0, 0):
                zero_wins += 1
            else:
                shifted_wins += 1
        print(f"  {b:>6} {len(cur):>6} {dx:>8} {dz:>8} "
              f"{f'({drow},{dcol})':>13} {err:>9.3f} {err0:>9.3f}"
              f"{'' if moved else '   (still)'}")

    # Counting which shift "wins" is too crude, and the fraction-of-spread test
    # is misleading: a smooth field gives a small residual whether the pairing is
    # right or not.
    #
    # The frames that decide it are the ones where the lattice carried a whole
    # number of columns along x and did not move in z. Only there can an index
    # shift express the carry at all; a mixed carry of 32 in x and 55 in z is
    # not a whole number of spacings in either axis, so no shift fits and the
    # comparison says nothing. On those clean frames, compare the error at the
    # best shift against the error at no shift:
    #
    #   heights carried with the lattice -> shifting cannot help, the two match
    #   heights from a world-fixed field -> shifting helps, and by a lot
    clean = [(e0, eb) for moved_x, moved_z, e0, eb in clean_frames
             if moved_z == 0 and abs(moved_x) >= 63]
    if clean:
        mean0 = sum(e0 for e0, _ in clean) / len(clean)
        meanb = sum(eb for _, eb in clean) / len(clean)
        print(f"  whole-column carries with no z movement: {len(clean)} frames")
        print(f"    error at no shift    {mean0:.3f}")
        print(f"    error at best shift  {meanb:.3f}"
              f"   ({100.0 * (mean0 - meanb) / mean0:.0f}% lower)")

    # Scale the residual against how much the heights vary in the first place.
    spread = 0.0
    for frame in order:
        ys = [y for _x, y, _z in frames[frame].values()]
        if len(ys) > 1:
            spread = max(spread, max(ys) - min(ys))
    mean_best = sum(best_errors) / len(best_errors) if best_errors else 0.0

    print()
    print(f"  height spread within a frame   {spread:.1f} world units")
    print(f"  mean error at the best shift   {mean_best:.1f}"
          f"  ({100.0 * mean_best / spread:.0f}% of the spread)" if spread else "")
    print()
    if zero_wins + shifted_wins == 0:
        print("  The lattice never moved in this capture, so nothing is settled.")
        print("  Capture more frames, or capture while the camera is moving.")
    elif len(clean) < 3:
        print(f"  VERDICT: undecided. Only {len(clean)} frame(s) carried a whole number")
        print("  of columns without moving in z, which is too few to conclude from --")
        print("  a single frame's 9% is noise. Capture more frames: how many qualify")
        print("  depends on which way the camera happens to be travelling.")
    elif (mean0 - meanb) > 0.15 * mean0:
        print("  VERDICT: resampled. On the frames where an index shift *can* express")
        print("  the carry, shifting cuts the error substantially -- the heights are")
        print("  attached to world position, not to the index. Pairing by index blends")
        print("  different water; it only looks harmless on mixed carries, where no")
        print("  integer shift fits and the comparison cannot tell the two apart.")
    elif clean:
        print("  VERDICT: carried. On whole-column carries, shifting the index does not")
        print("  reduce the error, so the heights travel with the lattice and index i")
        print("  keeps its meaning.")
    else:
        print("  VERDICT: undecided. No frame in this capture carried a whole number of")
        print("  columns without also moving in z, which is the only case that")
        print("  separates the two. Capture more frames.")


if __name__ == "__main__":
    raise SystemExit(main())
