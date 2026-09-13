"""Promote the sun directions you saved in the F1 menu into the course profiles.

The water sun editor's **Save to water_sun.json** writes to the per-user settings
folder (`%LOCALAPPDATA%\\WaveRace64Recomp` on Windows), which the game reads at
startup and which is outside the repository. A release carries none of it. This
copies each saved course's `sun_direction` into `assets/water/profiles.json`,
which ships beside the executable. Run it, look at the diff, commit.

    python tools/promote_water_sun.py            # promote
    python tools/promote_water_sun.py --dry-run  # show what it would change
    python tools/promote_water_sun.py --file X    # read a water_sun.json from elsewhere
    python tools/promote_water_sun.py --clear     # ... and delete the local file

Only `sun_direction` is touched, on the courses the file names. profiles.json is
written back in the formatting it already has (two-space JSON), so the diff is
the changed numbers and nothing else.

`--clear` deletes the local water_sun.json after promoting. Worth doing: the local
file overrides the profile, so while it exists a later change to the profile does
not show on this machine.
"""

import argparse
import json
import math
import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PROFILES = REPO / "assets" / "water" / "profiles.json"


def settings_directory():
    """The same directory wr64::settings_directory() picks (src/main.cpp)."""
    if Path("portable.txt").exists():
        return Path.cwd()
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA")
        if base:
            return Path(base) / "WaveRace64Recomp"
    else:
        base = os.environ.get("XDG_DATA_HOME")
        if base:
            return Path(base) / "WaveRace64Recomp"
        home = os.environ.get("HOME")
        if home:
            return Path(home) / ".local" / "share" / "WaveRace64Recomp"
    return Path.cwd()


def valid(sun):
    """The range src/water.cpp accepts; anything else it would ignore."""
    if not (isinstance(sun, list) and len(sun) == 4):
        return False
    if not all(isinstance(v, (int, float)) and math.isfinite(v) and -4 <= v <= 4 for v in sun):
        return False
    return sun[3] >= 0 and sun[0] ** 2 + sun[1] ** 2 + sun[2] ** 2 >= 0.01


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--file", type=Path, help="water_sun.json to read (default: the settings folder's)")
    ap.add_argument("--dry-run", action="store_true", help="show the changes without writing")
    ap.add_argument("--clear", action="store_true", help="delete the local water_sun.json after promoting")
    args = ap.parse_args()

    source = args.file or settings_directory() / "water_sun.json"
    if not source.exists():
        sys.exit(f"no water_sun.json at {source}\n"
                 f"Press F1 in a race, change the sun, and press Save to water_sun.json.")
    try:
        saved = json.loads(source.read_text(encoding="utf-8"))
        entries = saved["courses"]
    except (ValueError, KeyError, TypeError) as e:
        sys.exit(f"{source} is not a water_sun.json: {e}")

    raw = PROFILES.read_bytes().decode("utf-8")
    profiles = json.loads(raw)
    if json.dumps(profiles, indent=2) + "\n" != raw:
        sys.exit(f"{PROFILES} is not in two-space JSON formatting; promoting would reformat it")
    by_id = {course["id"]: course for course in profiles["courses"]}

    changed = 0
    for entry in entries:
        course_id, sun = entry.get("id"), entry.get("sun_direction")
        if course_id not in by_id or not valid(sun):
            print(f"skipped: {entry!r} (unknown course or sun out of range)")
            continue
        sun = [round(float(v), 4) for v in sun]
        course = by_id[course_id]
        if course["sun_direction"] == sun:
            print(f"{course['name']}: already {sun}")
            continue
        print(f"{course['name']}: {course['sun_direction']} -> {sun}")
        course["sun_direction"] = sun
        changed += 1

    if args.dry_run:
        print(f"dry run: {changed} course(s) would change")
        return
    if changed:
        PROFILES.write_bytes((json.dumps(profiles, indent=2) + "\n").encode("utf-8"))
    print(f"{changed} course(s) changed in {PROFILES.relative_to(REPO)}")
    if args.clear:
        source.unlink()
        print(f"deleted {source}")


if __name__ == "__main__":
    main()
