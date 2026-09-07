#!/usr/bin/env python3
"""Run sequential native water acceptance comparisons on one unchanged bundle.

Performance runs omit diagnostic GPU readbacks and recording. Physics runs
cover all three public quality levels at 30/60/120 Hz, with exact fixed-step
field comparisons between the High runs. Every child has a bounded stop tick.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from compare_water_traces import compare

ROOT = Path(__file__).resolve().parent.parent


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=["performance", "physics"], required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    executable = ROOT / "build-macos/WaveRace64Recomp.app/Contents/MacOS/WaveRace64Recomp"
    profiles = executable.parent.parent / "Resources/assets/water/profiles.json"
    identity = {"executable": digest(executable), "profiles": digest(profiles)}
    (out / "bundle.json").write_text(json.dumps(identity, indent=2) + "\n")
    runs = []

    def run(name, options):
        if identity != {"executable": digest(executable), "profiles": digest(profiles)}:
            raise RuntimeError("The bundle changed during validation")
        directory = out / name
        print(f"RUNNING {name}", flush=True)
        subprocess.run([sys.executable, str(ROOT / "tools/run_water_replay.py"),
                        "--output", str(directory), *options], cwd=ROOT, check=True)
        runs.append(directory)
        return directory

    def physics(reference, candidate, through):
        result = compare(reference / "state.csv", candidate / "state.csv", through)
        (candidate / "physics-comparison.json").write_text(json.dumps(result, indent=2) + "\n")
        if not result["passed"]:
            raise RuntimeError(f"Physics mismatch: {candidate}")
        print(f"PHYSICS {candidate.name}: {result['candidate_count']} exact checkpoints", flush=True)

    if args.suite == "performance":
        configurations = [(1080, 1, 60), (1440, 1, 60), (2160, 1, 60), (1080, 2, 60), (1080, 1, 120)]
        for height, players, fps in configurations:
            baseline = None
            for quality in ["original", "modern", "high"]:
                directory = run(f"{quality}-{height}-p{players}-{fps}", [
                    "--quality", quality, "--height", str(height), "--players", str(players),
                    "--fps", str(fps), "--msaa", "4", "--course", "1", "--mode", "trials",
                    "--through", "1430", "--script", str(ROOT / "tools/scripts/water/contact-review.ticks")])
                if baseline is None:
                    baseline = directory
                else:
                    physics(baseline, directory, 1410)
            subprocess.run([sys.executable, str(ROOT / "tools/summarize_water_benchmarks.py"),
                            *map(str, runs), "--output", str(out / "performance.json")], check=True)
    else:
        baseline = None
        fields = []
        for quality in ["original", "modern", "high"]:
            for fps in [60, 30, 120]:
                options = ["--quality", quality, "--fps", str(fps), "--msaa", "4", "--through", "2850"]
                if quality == "high":
                    options.append("--stats")
                directory = run(f"{quality}-{fps}", options)
                if baseline is None:
                    baseline = directory
                else:
                    physics(baseline, directory, 2850)
                if quality == "high":
                    rows = list(csv.DictReader((directory / "fields.csv").open()))
                    if not rows:
                        raise RuntimeError("No GPU field readbacks")
                    fields.append((directory, rows))
        reference, expected = fields[0]
        reports = []
        for directory, actual in fields[1:]:
            passed = actual == expected
            report = {"reference": str(reference), "candidate": str(directory), "passed": passed,
                      "reference_samples": len(expected), "candidate_samples": len(actual)}
            if not passed:
                report["first_difference"] = next((
                    {"index": index, "reference": left, "candidate": right}
                    for index, (left, right) in enumerate(zip(expected, actual)) if left != right
                ), {"reason": "sample count differs"})
            reports.append(report)
            (out / "fields.json").write_text(json.dumps(reports, indent=2) + "\n")
            if not passed:
                raise RuntimeError(f"Fixed-step field mismatch: {directory}")
    (out / "result.json").write_text(json.dumps({"passed": True, "runs": len(runs), "suite": args.suite}) + "\n")
    print(f"COMPLETE: {len(runs)} clean {args.suite} runs", flush=True)


if __name__ == "__main__":
    main()
