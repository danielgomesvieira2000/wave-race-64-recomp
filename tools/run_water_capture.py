#!/usr/bin/env python3
"""Capture a bounded, paused native water comparison with quality evidence.

Uses the installed screenshot and recording skill helpers. The replay owns
its process; this script never sends input to or stops another application.
"""
import argparse
import csv
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent


def latest(directory, filename):
    file = directory / filename
    if not file.exists():
        return {}
    rows = list(csv.DictReader(file.read_text().splitlines()))
    return rows[-1] if rows else {}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, default=ROOT / "build-macos/WaveRace64Recomp.app")
    parser.add_argument("--course", type=int, choices=range(9), required=True)
    parser.add_argument("--players", type=int, choices=[1, 2], default=1)
    parser.add_argument("--mode", choices=["trials", "championship"], default="trials")
    parser.add_argument("--script", type=Path, default=ROOT / "tools/scripts/water/contact-review.ticks")
    parser.add_argument("--moving-at", type=int, default=1230, help="Game tick to start moving capture; custom ticks require --motion-only")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--moving", action="store_true")
    parser.add_argument("--quality", choices=["original", "modern", "high"], default="high")
    parser.add_argument("--style", choices=["modern", "classic"], default="modern")
    parser.add_argument("--ripples", choices=["soft", "normal", "strong"], default="normal")
    parser.add_argument("--spray", choices=["off", "on"], default="on")
    parser.add_argument("--msaa", type=int, choices=[1, 2, 4, 8])
    parser.add_argument("--fps", type=int, choices=[30, 60, 120], default=60)
    parser.add_argument("--motion-only", action="store_true", help="Record this quality's moving replay without an automatic A/B flip")
    parser.add_argument("--stats", action="store_true", help="Record fixed-step wake and shoreline statistics (not a benchmark)")
    parser.add_argument("--debug", type=int, choices=range(15), default=0)
    parser.add_argument("--profiles", type=Path)
    args = parser.parse_args()
    if args.quality == "original" and not args.motion_only:
        parser.error("Original capture requires --motion-only")
    if args.moving_at < 0 or (args.moving_at != 1230 and not args.motion_only):
        parser.error("Custom nonnegative --moving-at requires --motion-only")
    expected_quality = {"original": 0, "modern": 1, "high": 2}[args.quality]
    out = args.output.resolve()
    skills = Path(os.environ.get("CODEX_HOME", str(Path.home() / ".codex"))) / "skills"
    capture = skills / "record-gui-tutorial/scripts/capture.py"
    screenshot = skills / "screenshot/scripts/take_screenshot.py"
    if not capture.is_file() or not screenshot.is_file():
        parser.error("The native screenshot and recording skill helpers are required")
    # Leave ten seconds between the High capture and automatic switch. Record
    # state on both sides of the actual screenshot so delayed helper startup
    # cannot silently turn an A/B comparison into two Original images.
    command = [sys.executable, str(ROOT / "tools/run_water_replay.py"),
               "--app", str(args.app.resolve()), "--quality", args.quality, "--fps", str(args.fps), "--course", str(args.course),
               "--style", args.style, "--ripples", args.ripples, "--spray", args.spray,
               "--players", str(args.players), "--mode", args.mode,
               "--script", str(args.script.resolve()),
               "--through", str(max(1420,args.moving_at+190)) if args.motion_only else "1770", "--output", str(out)]
    if not args.motion_only:
        command += ["--compare-at", "1640"]
    if args.stats:
        command.append("--stats")
    if args.msaa:
        command += ["--msaa", str(args.msaa)]
    command += ["--debug", str(args.debug)]
    if args.profiles:
        command += ["--profiles", str(args.profiles.resolve())]
    run = subprocess.Popen(command, cwd=ROOT)
    window = None
    records = []
    captured = set()
    recorder = None
    motion = None
    log = None
    next_inventory = 0
    while run.poll() is None:
        if window is None and (out / "process.json").exists() and time.monotonic() >= next_inventory:
            pid = json.loads((out / "process.json").read_text())["pid"]
            listing = subprocess.run([sys.executable, str(capture), "list", "--app", "Wave Race"], capture_output=True, text=True)
            next_inventory = time.monotonic() + 2
            for line in listing.stdout.splitlines():
                try:
                    row = json.loads(line)
                except ValueError:
                    continue
                if row.get("pid") == pid and row.get("width", 0) > 400:
                    window = row["window_id"]
                    break
        tick = int(latest(out, "state.csv").get("tick", 0))
        if window and (args.moving or args.motion_only) and tick >= args.moving_at and recorder is None:
            log = (out / "capture.log").open("w")
            motion = {"before": latest(out, "frames.csv"), "state_before": latest(out, "state.csv")}
            recorder = subprocess.Popen([sys.executable, str(capture), "record", "--window", str(window),
                "--duration", "6", "--fps", str(args.fps), "--scale", "1", "--output", str(out / f"{args.quality}-moving-source.mp4")],
                stdout=log, stderr=subprocess.STDOUT)
        if recorder and recorder.poll() is not None and "after" not in motion:
            motion.update(after=latest(out, "frames.csv"), state_after=latest(out, "state.csv"))
            motion["valid"] = recorder.returncode == 0 and all(int(motion[key].get("quality", -1)) == expected_quality for key in ("before", "after"))
            (out / "motion-capture.json").write_text(json.dumps(motion, indent=2) + "\n")
        comparisons = [] if args.motion_only else [(args.quality, 1440, expected_quality), ("original", 1680, 0)]
        for name, threshold, quality in comparisons:
            if window is None or tick < threshold or name in captured:
                continue
            before = latest(out, "frames.csv")
            state_before = latest(out, "state.csv")
            shot = subprocess.run([sys.executable, str(screenshot), "--window-id", str(window), "--mode", "temp"],
                                  capture_output=True, text=True)
            after = latest(out, "frames.csv")
            state_after = latest(out, "state.csv")
            files = [Path(line) for line in shot.stdout.splitlines() if Path(line).is_file()]
            valid = bool(files) and all(int(row.get("quality", -1)) == quality for row in (before, after))
            if files:
                shutil.copyfile(files[-1], out / f"{name}-paused.png")
            records.append({"name": name, "valid": valid, "before": before, "after": after,
                            "state_before": state_before, "state_after": state_after,
                            "capture_returncode": shot.returncode})
            captured.add(name)
            (out / "comparison-captures.json").write_text(json.dumps(records, indent=2) + "\n")
        time.sleep(.25)
    if recorder:
        recorder.wait(timeout=20)
        log.close()
        # The native recorder emits variable timestamps. Normalize the review
        # copy to the requested rate and decode the complete file to catch broken output.
        source, target = out / f"{args.quality}-moving-source.mp4", out / f"{args.quality}-moving.mp4"
        subprocess.run(["ffmpeg", "-v", "error", "-i", str(source), "-vf", f"fps={args.fps}", "-c:v", "libx264",
                        "-crf", "18", "-pix_fmt", "yuv420p", "-an", str(target)], check=True)
        subprocess.run(["ffmpeg", "-v", "error", "-i", str(target), "-f", "null", "-"], check=True)
    valid = run.returncode == 0 and (args.motion_only or (len(records) == 2 and all(row["valid"] for row in records)))
    if args.moving or args.motion_only:
        valid = valid and motion is not None and motion.get("valid", False)
    print(json.dumps({"valid_comparison": valid, "output": str(out)}))
    return 0 if valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
