"""Which things the game stops drawing at a distance, and which it does not.

There is no global draw distance in Wave Race 64. The far plane sits about
twenty times further out than anything drawn, and each kind of object is culled
-- or not -- by its own code with its own rule. So the only way to know what a
draw-distance setting could reach is to measure each kind separately.

**Reach against opportunity.** The first version of this watched the furthest
instance of each kind per frame and called a kind culled when that maximum held
steady. That works for the buoys, which line the whole course so that some buoy
always sits at the limit, and it is worthless for everything else: a lone object
drawn in 55 frames has a per-frame maximum that is just where it happens to be,
and it reads as a perfect ceiling. Four kinds were reported culled at exactly
2572 on that basis, and all four are drawn at the world origin under no matrix
at all -- their "distance" was the camera's distance from (0,0,0).

The test that generalises uses the fact that most of this geometry is **static**:
its matrix is loaded from ROM, so the positions it is drawn at are a fixed set of
sites. The camera is recorded in every row, so for every frame of the run the
distance to each site is known **whether or not the object was drawn**. That
gives two numbers:

    reach        the furthest the kind was ever actually drawn
    opportunity  the furthest any of its sites ever was from the camera

    reach ~ opportunity   the run never got far enough away to find a limit,
                          or there is none: it is drawn as far as it is put
    reach << opportunity  it was within range hundreds of times and never once
                          drawn beyond `reach` -- that is a distance cull, and
                          `reach` is the number to go hunting for

The hit rate inside `reach` is reported alongside, because it is what separates a
cull from ordinary invisibility: an object drawn in most frames where it is close
and in none where it is far is culled by distance; one drawn in a tenth of its
close frames is being gated by something else -- facing, occlusion, a room -- and
this cannot tell which.

**Let the attract demo drive.** A blind input script cannot see the shore: the
first attempt beached the craft and the race was over in twenty seconds, 443
frames. The demo drives the course properly and moves on to another course by
itself, so an unattended run covers several courses without a script at all --
which is also what the plan asks for, since a verdict that does not reproduce on
a second course is not a finding. Every row carries its course number and the
analysis is per course.

    WR64_DISTANCE_CSV=census.csv WR64_3D_TRACE_FRAMES=3000 <the port> <rom.z64>
    python tools/render_distance_census.py census.csv

Anything flagged culled is then worth hunting for the number behind it; see
include/wr64/drawdistance.h. Anything flagged uncalled has nothing for a setting
to scale, and saying so is as useful as finding a limit.
"""

import argparse
import statistics
import sys
from collections import defaultdict
from pathlib import Path

# A kind seen in fewer frames than this has too little evidence either way.
MIN_FRAMES = 30

# How far from the world origin a site has to be to count as a real position.
# Anything drawn at the origin is drawn under no world matrix -- the HUD, the
# sky, a full-screen effect -- and the census has nothing to say about it.
ORIGIN_RADIUS = 200.0

# A kind whose distinct positions outnumber this fraction of its draws is not
# standing still: another racer, a spray, a piece of the player's craft. Its
# sites are not fixed, so the opportunity its geometry never took cannot be
# computed and the reach test does not apply.
MOVING_SITE_RATIO = 0.5

# reach as a fraction of opportunity. Above this the run simply never put the
# object far enough away for a limit to show.
NO_LIMIT_RATIO = 0.90

# How many (frame, site) pairs must sit beyond the reach before "never drawn
# beyond it" means anything.
MIN_BEYOND = 30

# Every camera against every site is frames x sites per kind, and a long run has
# thousands of each -- billions of distances for an answer that does not need
# them. Both are thinned to an even stride instead, which keeps the extremes of
# a course-long camera track and a course-wide set of sites while bounding the
# work. `beyond` is scaled back up so its threshold still means pairs.
CAMERA_SAMPLE = 400
SITE_SAMPLE = 200


def thin(items, limit):
    """An evenly spaced subset, at most `limit` long, in a stable order.

    Even spacing rather than the first N: the first N cameras of a run are one
    corner of the course, and a subset that never leaves it would report that
    nothing ever got far away.
    """
    items = list(items)
    if len(items) <= limit:
        return items, 1
    stride = (len(items) + limit - 1) // limit
    return items[::stride], stride

ORDER = {"CULLED": 0, "unclear": 1, "not culled": 2, "never far": 3,
         "moves": 4, "at origin": 5}


def load(path: Path):
    """course -> (kind -> {frame: [(x, y, z, distance)]}, frame -> (camx, camz)).

    Split by course, because both halves of the test are per-course: a kind's
    sites are the positions *this* course puts it at, and the camera track is
    the one *this* course's run produced. Merging two courses would compare a
    reach measured on one against an opportunity taken on the other.
    """
    rows = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    cameras = defaultdict(dict)
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#") or line.startswith("frame,"):
            continue
        parts = line.split(",")
        if len(parts) != 10:
            continue          # a run killed mid-write leaves a short last line
        frame, course, dlist, texture, x, y, z, distance, camx, camz = parts
        try:
            f = int(frame)
            c = int(course)
            rows[c][(dlist, texture)][f].append(
                (float(x), float(y), float(z), float(distance)))
            cameras[c][f] = (float(camx), float(camz))
        except ValueError:
            continue
    return rows, cameras


def sites_of(per_frame):
    """The distinct positions a kind was drawn at, and how many draws in all."""
    seen = set()
    draws = 0
    for instances in per_frame.values():
        for x, y, z, _d in instances:
            seen.add((x, y, z))
            draws += 1
    return seen, draws


def classify(sites, draws):
    """Whether the reach test can say anything about this kind at all."""
    if all(abs(x) < ORIGIN_RADIUS and abs(z) < ORIGIN_RADIUS for x, _y, z in sites):
        return "at origin"
    if len(sites) > MOVING_SITE_RATIO * draws:
        return "moves"
    return None


def opportunity(sites, cameras, reach):
    """How far the sites got from the camera, and how often, over the whole run.

    Returns (furthest, pairs beyond the reach). A pair is one frame against one
    site: the distance the object would have been drawn at, had nothing stopped
    it.
    """
    some_cameras, camera_stride = thin(sorted(cameras.items()), CAMERA_SAMPLE)
    some_sites, site_stride = thin(sorted(sites), SITE_SAMPLE)
    squared = reach * reach
    furthest_sq = 0.0
    beyond = 0
    for _frame, (camx, camz) in some_cameras:
        for x, _y, z in some_sites:
            d = (x - camx) ** 2 + (z - camz) ** 2
            if d > furthest_sq:
                furthest_sq = d
            if d > squared:
                beyond += 1
    return furthest_sq ** 0.5, beyond * camera_stride * site_stride


def hit_rate(sites, cameras, per_frame, reach):
    """Of the frames where a site was within the reach, how many drew the kind.

    Counted per frame rather than per site, since one draw is all it takes to
    show the object was not being culled in that frame.
    """
    some_cameras, _ = thin(sorted(cameras.items()), CAMERA_SAMPLE)
    some_sites, _ = thin(sorted(sites), SITE_SAMPLE)
    squared = reach * reach
    close = 0
    drawn = 0
    for frame, (camx, camz) in some_cameras:
        for x, _y, z in some_sites:
            if (x - camx) ** 2 + (z - camz) ** 2 <= squared:
                close += 1
                if frame in per_frame:
                    drawn += 1
                break
    return (drawn / close) if close else 0.0


def analyse(rows, cameras, min_frames, include_all):
    """Every kind of one course, judged."""
    results = []
    for (dlist, texture), per_frame in rows.items():
        if len(per_frame) < min_frames and not include_all:
            continue
        sites, draws = sites_of(per_frame)
        reach = max(d for instances in per_frame.values() for *_p, d in instances)
        excuse = classify(sites, draws)
        if excuse is not None:
            results.append((excuse, reach, 0.0, 0.0, len(per_frame), len(sites),
                            0, dlist, texture))
            continue

        furthest, beyond = opportunity(sites, cameras, reach)
        hit = hit_rate(sites, cameras, per_frame, reach)

        if reach * NO_LIMIT_RATIO <= furthest <= reach / NO_LIMIT_RATIO:
            call = "never far"
        elif reach < NO_LIMIT_RATIO * furthest and beyond >= MIN_BEYOND:
            call = "CULLED" if hit >= 0.5 else "unclear"
        else:
            call = "not culled"
        results.append((call, reach, furthest, hit, len(per_frame), len(sites),
                        beyond, dlist, texture))
    return results


def report(results, wanted, show_all):
    """Print one course's table, and return its verdict per kind."""
    results.sort(key=lambda r: (ORDER[r[0]], -r[1]))
    print(f"  {'verdict':<10} {'reach':>7} {'furthest':>9} {'drawn<=reach':>13} "
          f"{'frames':>7} {'sites':>6} {'beyond':>8}  list / texture")
    for call, reach, furthest, hit, nframes, nsites, beyond, dlist, texture in results:
        if not show_all and call.lower() not in wanted:
            continue
        far = f"{furthest:>9.0f}" if furthest else f"{'-':>9}"
        rate = f"{hit:>12.0%}" if furthest else f"{'-':>12}"
        print(f"  {call:<10} {reach:>7.0f} {far} {rate} "
              f"{nframes:>7} {nsites:>6} {beyond:>8}  {dlist} {texture}")

    tally = defaultdict(int)
    for r in results:
        tally[r[0]] += 1
    print("  " + ", ".join(f"{tally[k]} {k}" for k in ORDER if tally[k]))

    culled = [r for r in results if r[0] == "CULLED"]
    if culled:
        reaches = sorted(r[1] for r in culled)
        shown = ", ".join(f"{r:.0f}" for r in reaches[:12])
        more = " ..." if len(reaches) > 12 else ""
        print(f"  reaches: {shown}{more}   median {statistics.median(reaches):.0f}")
    return {(r[-2], r[-1]): r[0] for r in results}


def pool(paths):
    """Merge several captures, keeping each one's frames its own.

    Frames are numbered per capture, so a second file's frame 0 would land on
    the first file's frame 0 and silently merge two different moments.
    """
    rows = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    cameras = defaultdict(dict)
    offset = 0
    for path in paths:
        part_rows, part_cameras = load(path)
        highest = 0
        for course, per_kind in part_rows.items():
            for kind, per_frame in per_kind.items():
                for frame, instances in per_frame.items():
                    rows[course][kind][frame + offset].extend(instances)
                    highest = max(highest, frame)
        for course, per_frame in part_cameras.items():
            for frame, cam in per_frame.items():
                cameras[course][frame + offset] = cam
        offset += highest + 1
    return rows, cameras


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("csv", type=Path, nargs="+",
                        help="one or more captures; courses are pooled by number")
    parser.add_argument("--min-frames", type=int, default=MIN_FRAMES)
    parser.add_argument("--all", action="store_true",
                        help="include kinds seen in too few frames to judge")
    parser.add_argument("--show", default="culled,unclear",
                        help="verdicts to print; 'all' for every one")
    parser.add_argument("--course", type=int, action="append",
                        help="only this course number; repeatable")
    args = parser.parse_args()

    rows, cameras = pool(args.csv)
    if not rows:
        sys.exit("no rows with the ten-column layout. A capture from before the "
                 "course and camera columns were added cannot be analysed this "
                 "way; re-run the capture.")

    wanted = {w.strip().lower() for w in args.show.split(",")}
    show_all = "all" in wanted

    verdicts = {}
    for course in sorted(rows):
        if args.course and course not in args.course:
            continue
        per_course = cameras[course]
        camxs = [c[0] for c in per_course.values()]
        camzs = [c[1] for c in per_course.values()]
        print()
        print(f"=== course {course}: {len(per_course)} frames, "
              f"{len(rows[course])} kinds, camera x {min(camxs):.0f}..{max(camxs):.0f},"
              f" z {min(camzs):.0f}..{max(camzs):.0f} ===")
        results = analyse(rows[course], per_course, args.min_frames, args.all)
        if not results:
            print("  nothing seen in enough frames to judge")
            continue
        verdicts[course] = report(results, wanted, show_all)

    # The plan's own rule: a verdict that is not reproducible on a second course
    # is not a finding. Where a kind was judged on more than one course, say
    # whether the courses agree.
    shared = [c for c in sorted(verdicts) if verdicts[c]]
    if len(shared) > 1:
        first = shared[0]
        agree, differ = 0, []
        for kind, call in verdicts[first].items():
            others = [verdicts[c][kind] for c in shared[1:] if kind in verdicts[c]]
            if not others:
                continue
            if all(o == call for o in others):
                agree += 1
            else:
                differ.append((kind, call, others))
        print()
        print(f"=== across courses {', '.join(str(c) for c in shared)} ===")
        print(f"  {agree} kind(s) judged the same on every course they appear on, "
              f"{len(differ)} differ")
        for kind, call, others in differ[:15]:
            print(f"    {kind[0]} {kind[1]}: {call} vs {', '.join(others)}")


if __name__ == "__main__":
    raise SystemExit(main())
