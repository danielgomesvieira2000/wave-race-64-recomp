#!/usr/bin/env python3
"""Read a transform-pairing log written by RT64 under WR64_PAIRING_LOG.

The log says, for every interpolated frame, which previous-frame transform each
world transform was paired with and how far apart the two are. See
tools/patch_rt64.py for the line format and docs/TRANSFORM-PAIRING.md for what
the numbers mean.

    python tools/pairing_log.py pairing.log
    python tools/pairing_log.py pairing.log --over 100
    python tools/pairing_log.py pairing.log --frames 800-900 --over 50 --lines
    python tools/pairing_log.py pairing.log --calls
    python tools/pairing_log.py pairing.log --scenes

Default output: per-run totals, the distribution of the jumps every pair
made, and the frames in which any pair jumped further than --over world units
(100 by default) with the offending pairs listed. --calls groups the pairs by
RT64's draw-call hash instead, which is how a repeated object -- the buoys, a
racer's limbs -- is found: the same hash every frame, and a wide matrix range
if the call spans many matrices.

--scenes reports the camera pairing instead: for every frame, which previous
scene (camera and framebuffer) each scene was paired with, and how many pairs
joined scenes on different framebuffer slots or different parts of the screen.
In a split-screen race those are views drawn through the other view's camera,
which is what the two-player "burst" was. Scenes that did not overlap are
listed by frame with their scissor regions.
"""

import argparse
import collections
import re
import sys

T_LINE = re.compile(
    r"^T (\d+) (\d+) (id|auto) id=([0-9A-F]+) call=([0-9A-F]+) range=(\d+)-(\d+) "
    r"cur=([-\d.]+),([-\d.]+),([-\d.]+) prev=([-\d.]+),([-\d.]+),([-\d.]+) "
    r"jump=([-\d.]+) lerp=(\d) pvel=([-\d.]+)$")
U_LINE = re.compile(r"^U (\d+) id=([0-9A-F]+) call=([0-9A-F]+) range=(\d+)-(\d+) cur=([-\d.]+),([-\d.]+),([-\d.]+)$")
V_LINE = re.compile(r"^V (persp|ortho) cur=(\d+)/(\d+) fb=(\d+) n=(\d+) scissor=(-?\d+),(-?\d+),(-?\d+),(-?\d+) "
                    r"prev=(\d+)/(\d+) fb=(\d+) n=(\d+) scissor=(-?\d+),(-?\d+),(-?\d+),(-?\d+) diff=([-\d.]+)$")
S_LINE = re.compile(r"^S frame=(\d+) total=(\d+) paired=(\d+) j50=(\d+) j100=(\d+) j200=(\d+) max=([-\d.]+) refused=(\d+)$")


class Pair:
    __slots__ = ("t", "prev", "path", "mid", "call", "lo", "hi", "cur", "prevpos", "jump", "lerp", "pvel", "line")

    def __init__(self, m, line):
        self.t = int(m.group(1))
        self.prev = int(m.group(2))
        self.path = m.group(3)
        self.mid = m.group(4)
        self.call = m.group(5)
        self.lo = int(m.group(6))
        self.hi = int(m.group(7))
        self.cur = tuple(float(m.group(i)) for i in (8, 9, 10))
        self.prevpos = tuple(float(m.group(i)) for i in (11, 12, 13))
        self.jump = float(m.group(14))
        self.lerp = m.group(15) == "1"
        self.pvel = float(m.group(16))
        self.line = line


class Frame:
    __slots__ = ("number", "wall", "pairs", "unpaired", "summary")

    def __init__(self, number):
        self.number = number
        self.wall = None
        self.pairs = []
        self.unpaired = []
        self.summary = None


def read(path, lo, hi):
    frames = []
    frame = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.rstrip("\n")
            if line.startswith("F "):
                fields = line[2:].split()
                number = int(fields[0])
                frame = Frame(number) if lo <= number <= hi else None
                if frame is not None and len(fields) > 1 and fields[1].startswith("wall="):
                    frame.wall = int(fields[1][5:])
                if frame is not None:
                    frames.append(frame)
                continue
            if frame is None:
                continue
            if line.startswith("T "):
                m = T_LINE.match(line)
                if m:
                    frame.pairs.append(Pair(m, line))
            elif line.startswith("U "):
                m = U_LINE.match(line)
                if m:
                    frame.unpaired.append(line)
            elif line.startswith("S "):
                m = S_LINE.match(line)
                if m:
                    frame.summary = tuple(int(m.group(i)) for i in (2, 3, 4, 5, 6)) + (float(m.group(7)), int(m.group(8)))
    return frames


def percentile(values, p):
    if not values:
        return 0.0
    values = sorted(values)
    k = min(len(values) - 1, max(0, int(round(p / 100.0 * (len(values) - 1)))))
    return values[k]


def region(x0, y0, x1, y1):
    """A scissor from the log, in quarter pixels; None for a scene that drew nothing."""
    if x0 > x1 or y0 > y1:
        return None
    return (x0, y0, x1, y1)


def overlapping(a, b):
    """The rule the renderer applies: both empty, or overlapping by half the smaller."""
    if a is None or b is None:
        return a is None and b is None
    ix = min(a[2], b[2]) - max(a[0], b[0])
    iy = min(a[3], b[3]) - max(a[1], b[1])
    if ix <= 0 or iy <= 0:
        return False
    area = lambda r: (r[2] - r[0]) * (r[3] - r[1])
    return 2 * ix * iy >= min(area(a), area(b))


def show(r):
    return "nothing drawn" if r is None else "(%d,%d)-(%d,%d)" % (r[0] // 4, r[1] // 4, r[2] // 4, r[3] // 4)


def scenes(path, lo, hi, max_frames):
    frame = None
    pairs = 0
    crossed = []
    unpaired = 0
    per_frame = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.rstrip("\n")
            if line.startswith("F "):
                frame = int(line[2:].split()[0])
                continue
            if frame is None or not (lo <= frame <= hi) or not line.startswith("V "):
                continue
            m = V_LINE.match(line)
            if not m:
                continue
            g = m.groups()
            cur = region(*map(int, g[5:9]))
            prev = region(*map(int, g[13:17]))
            pairs += 1
            count = per_frame.setdefault(frame, [0, int(g[2])])
            count[0] += 1
            if g[3] != g[11] or not overlapping(cur, prev):
                crossed.append((frame, g[0], g[3], cur, g[11], prev, float(g[17])))
    for matched, total in per_frame.values():
        if matched < total:
            unpaired += 1
    print("%d frames, %d scene pairs; %d paired across framebuffer slots or screen regions; "
          "%d frames with a scene left unpaired (a new view, or no compatible one)"
          % (len(per_frame), pairs, len(crossed), unpaired))
    shown = set()
    for fr, kind, cfb, cur, pfb, prev, diff in crossed:
        if fr not in shown:
            if len(shown) >= max_frames:
                print("  ...")
                break
            shown.add(fr)
        print("  frame %d %s: fb %s %s  <-  fb %s %s  (difference %.1f)" % (fr, kind, cfb, show(cur), pfb, show(prev), diff))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--over", type=float, default=100.0, help="report pairs that jumped further than this (world units)")
    ap.add_argument("--frames", default=None, help="only frames A-B of the log")
    ap.add_argument("--lines", action="store_true", help="print the offending log lines in full")
    ap.add_argument("--calls", action="store_true", help="group the pairs by draw-call hash")
    ap.add_argument("--scenes", action="store_true", help="report how cameras were paired between frames")
    ap.add_argument("--max-frames", type=int, default=40, help="how many offending frames to list")
    args = ap.parse_args()

    lo, hi = 0, 1 << 62
    if args.frames:
        a, _, b = args.frames.partition("-")
        lo = int(a or 0)
        hi = int(b or hi)

    if args.scenes:
        scenes(args.log, lo, hi, args.max_frames)
        return

    frames = read(args.log, lo, hi)
    if not frames:
        sys.exit("no frames in that range")

    jumps = [p.jump for fr in frames for p in fr.pairs]
    moving = [j for j in jumps if j > 0.5]
    total = sum(len(fr.pairs) + len(fr.unpaired) for fr in frames)
    paired = sum(len(fr.pairs) for fr in frames)
    refused = sum(fr.summary[6] for fr in frames if fr.summary)
    by_id = sum(1 for fr in frames for p in fr.pairs if p.path == "id")
    print(f"frames {frames[0].number}-{frames[-1].number}: {len(frames)} frames, "
          f"{total / len(frames):.1f} transforms a frame, {paired / len(frames):.1f} paired "
          f"({by_id / len(frames):.1f} by id), {refused / len(frames):.2f} candidates refused a frame")
    print(f"jumps of every pair: p50 {percentile(jumps, 50):.1f}  p90 {percentile(jumps, 90):.1f}  "
          f"p99 {percentile(jumps, 99):.1f}  p99.9 {percentile(jumps, 99.9):.1f}  max {max(jumps) if jumps else 0:.1f}")
    if moving:
        print(f"pairs that moved at all ({len(moving)}): p50 {percentile(moving, 50):.1f}  p90 {percentile(moving, 90):.1f}  "
              f"p99 {percentile(moving, 99):.1f}  max {max(moving):.1f}")
    for limit in (25, 50, 100, 200, 400):
        n = sum(1 for j in jumps if j > limit)
        f = sum(1 for fr in frames if any(p.jump > limit for p in fr.pairs))
        print(f"  over {limit:>4}: {n:>6} pairs in {f:>5} frames")

    if args.calls:
        print()
        print("by draw call (hash, pairs, pairs over the limit, matrix range width, first frame, last frame):")
        calls = collections.OrderedDict()
        for fr in frames:
            for p in fr.pairs:
                c = calls.setdefault(p.call, [0, 0, 0, fr.number, fr.number])
                c[0] += 1
                if p.jump > args.over:
                    c[1] += 1
                c[2] = max(c[2], p.hi - p.lo + 1)
                c[4] = fr.number
        for h, c in sorted(calls.items(), key=lambda kv: -kv[1][1]):
            print(f"  {h}  {c[0]:>7}  {c[1]:>6}  width {c[2]:>3}  frames {c[3]}-{c[4]}")
        return

    print()
    print(f"frames with a pair over {args.over:g} units:")
    shown = 0
    for fr in frames:
        bad = [p for p in fr.pairs if p.jump > args.over]
        if not bad:
            continue
        shown += 1
        if shown > args.max_frames:
            print("  ...")
            break
        s = fr.summary
        summary = f"total {s[0]} paired {s[1]} refused {s[6]}" if s else ""
        wall = f" wall={fr.wall}" if fr.wall is not None else ""
        print(f"  frame {fr.number}{wall}: {len(bad)} pairs over, largest {max(p.jump for p in bad):.1f}  ({summary})")
        for p in sorted(bad, key=lambda p: -p.jump)[:12]:
            if args.lines:
                print("    " + p.line)
            else:
                print(f"    t{p.t:<4} <- {p.prev:<4} {p.path:<4} call {p.call[-8:]} range {p.lo}-{p.hi:<4} "
                      f"jump {p.jump:>7.1f}  lerp {int(p.lerp)}  pvel {p.pvel:>6.1f}  "
                      f"cur ({p.cur[0]:.0f},{p.cur[1]:.0f},{p.cur[2]:.0f}) prev ({p.prevpos[0]:.0f},{p.prevpos[1]:.0f},{p.prevpos[2]:.0f})")
    if shown == 0:
        print("  none")


if __name__ == "__main__":
    try:
        main()
    except (BrokenPipeError, OSError):
        # The reader closed the pipe (head, less); nothing to report.
        pass
