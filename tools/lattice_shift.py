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


def load(path: Path):
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
        if len(parts) != 6:
            continue          # the run is killed mid-write; the last line is short
        frame, block, index, x, y, z = (int(v) for v in parts)
        frames[frame][(block, index)] = (x, y, z)
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

    frames = load(args.csv)
    if len(frames) < 2:
        sys.exit(f"{args.csv}: need at least two frames, found {len(frames)}")

    order = sorted(frames)
    print(f"{len(order)} frames, {len(frames[order[0]])} vertices in the first\n")
    print(f"  {'frame':>6} {'verts':>6} {'moved x':>8} {'moved z':>8} "
          f"{'best row,col':>13} {'err@best':>9} {'err@0':>9}")

    zero_wins = 0
    shifted_wins = 0
    best_errors = []
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
        moved = dx != 0 or dz != 0
        if moved:
            if (drow, dcol) == (0, 0):
                zero_wins += 1
            else:
                shifted_wins += 1
        print(f"  {b:>6} {len(cur):>6} {dx:>8} {dz:>8} "
              f"{f'({drow},{dcol})':>13} {err:>9.3f} {err0:>9.3f}"
              f"{'' if moved else '   (still)'}")

    # The question is not only *which* shift fits best but whether any shift
    # fits at all. Scale the residual against how much the heights vary in the
    # first place: a best-case error that is a large fraction of the spread
    # means no index mapping preserves the surface, whatever the best shift is.
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
    elif spread and mean_best > 0.15 * spread:
        print("  VERDICT: neither. No index mapping preserves the heights -- the best")
        print("  shift still leaves a large fraction of the height spread as error, on")
        print("  every frame. The surface genuinely changes from frame to frame, so")
        print("  pairing by index blends different water however the indices are")
        print("  aligned. A renderer that interpolates per index is interpolating")
        print("  between unrelated samples.")
    elif shifted_wins > zero_wins:
        print(f"  VERDICT: resampled. {shifted_wins} of {zero_wins + shifted_wins}")
        print("  carrying frames match best at a nonzero index shift, which is what a")
        print("  world-fixed field sampled by a moving lattice looks like.")
    else:
        print(f"  VERDICT: carried. {zero_wins} of {zero_wins + shifted_wins} carrying")
        print("  frames match best at shift zero, and the residual is small, so index i")
        print("  keeps its meaning and pairing by index is sound.")


if __name__ == "__main__":
    raise SystemExit(main())
