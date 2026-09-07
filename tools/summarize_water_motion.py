#!/usr/bin/env python3
"""Summarize one water motion replay, or compare a before/after pair.

The renderer builds each draw once before frame matching (weight 1, zero
velocities), then again for each rendered presentation. Match motion rows to
frames.csv and remove only that verified setup row. CPU timestamps measure
batched render preparation, not display presentation or visual smoothness.

Example:
  python3 tools/summarize_water_motion.py BEFORE AFTER --course 1 \
      --require-spatial-match --output build/water-motion-comparison.json
"""
import argparse
from collections import Counter, defaultdict
import csv
import json
import math
from pathlib import Path
import statistics


def read_csv(path):
    with path.open(newline="") as source:
        return [{name: float(value) for name, value in row.items()}
                for row in csv.DictReader(source)]


def distribution(values):
    values = sorted(values)
    if not values:
        return {"count": 0, "mean": None, "median": None, "p95": None, "max": None}
    return {"count": len(values), "mean": statistics.mean(values),
            "median": statistics.median(values),
            "p95": values[math.ceil(len(values) * .95) - 1], "max": values[-1]}


def interval_key(row):
    return (int(row["course"]), int(row["generation"]), int(row["view"]),
            int(row["draw_index"]), round(row["prev_time"], 6), round(row["current_time"], 6))


def vertical_rms(row):
    return math.sqrt(max(0, row["position_velocity_rms"] ** 2 - row["xz_velocity_rms"] ** 2))


def summarize(directory, course):
    directory = directory.resolve()
    settings = json.loads((directory / "settings.json").read_text())
    result = json.loads((directory / "result.json").read_text())
    motion = [row for row in read_csv(directory / "motion.csv") if int(row["course"]) == course]
    frames = [row for row in read_csv(directory / "frames.csv")
              if int(row["course"]) == course and row["water_views"] > 0]
    errors = []
    if not result.get("passed") or result.get("exit_before_cleanup") != 0:
        errors.append("Replay did not reach a clean successful process exit.")
    if not motion or not frames:
        errors.append("Selected course has no motion rows or no rendered water frame records.")
    finite = all(math.isfinite(value) for row in motion + frames for value in row.values())
    if not finite:
        raise ValueError(f"{directory}: non-finite numeric trace values")

    frame_counts = Counter((int(row["submission"]), int(row["quality"])) for row in frames)
    groups = defaultdict(list)
    for row in motion:
        groups[(int(row["submission"]), int(row["quality"]), int(row["view"]), int(row["draw_index"]))].append(row)
    actual, representatives = [], []
    setup_rows = dry_only_groups = 0
    mismatch_groups = []
    weight_patterns = Counter()
    complete = incomplete = positive_groups = single_sample_groups = 0
    within_spacing, boundary_spacing = [], []
    first_stream = min((key[2:] for key in groups), default=None)
    stream = []
    for key, rows in groups.items():
        expected = frame_counts[key[:2]]
        first = rows[0]
        is_setup = (abs(first["interpolation_weight"] - 1) < 1e-6
                    and first["position_moving_vertices"] == 0 and first["uv_moving_vertices"] == 0)
        if len(rows) == expected + 1 and is_setup:
            rows = rows[1:]
            setup_rows += 1
        if not expected and not rows:
            dry_only_groups += 1
            continue
        if len(rows) != expected:
            mismatch_groups.append({"group": list(key), "motion_rows_after_setup": len(rows), "frame_records": expected})
        if not rows:
            continue
        actual.extend(rows)
        weights = [row["interpolation_weight"] for row in rows]
        if any(weight < -1e-6 or weight > 1 + 1e-6 for weight in weights):
            errors.append(f"Out-of-range interpolation weight in group {key}.")
        if any(b <= a + 1e-7 for a, b in zip(weights, weights[1:])):
            errors.append(f"Repeated or decreasing actual presentation weights in group {key}.")
        for row in rows:
            target = row["prev_time"] + (row["current_time"] - row["prev_time"]) * row["interpolation_weight"]
            if row["current_time"] < row["prev_time"] or abs(target - row["camera_time"]) > 1e-5:
                errors.append(f"Presentation clock does not follow the game interval and weight in group {key}.")
                break
        duration = rows[-1]["current_time"] - rows[-1]["prev_time"]
        if duration > 1e-6:
            positive_groups += 1
            representatives.append(rows[-1])
            # A 30 Hz presentation over 20 Hz game updates legitimately
            # alternates [1/3, 1] and [2/3]. Check spacing and covered interval
            # boundaries instead of demanding a rounded per-update count.
            period = 1 / settings["fps"]
            # Compare in seconds so subtraction of float game timestamps
            # does not magnify rounding error at nonintegral rate ratios.
            is_complete = (weights[0] * duration <= period + 1e-5
                           and (1 - weights[-1]) * duration < period - 1e-5
                           and all(abs((b - a) * duration - period) < 1e-5 for a, b in zip(weights, weights[1:])))
            complete += is_complete
            incomplete += not is_complete
            single_sample_groups += len(weights) == 1 and abs(weights[0] - 1) < 1e-6
            weight_patterns[",".join(f"{weight:.6f}" for weight in weights)] += 1
        if key[2:] == first_stream:
            stream.extend((int(row["cpu_time_us"]), key[0]) for row in rows)
    if mismatch_groups:
        errors.append("Motion rows do not reconcile with rendered frame counts after setup removal.")
    if not representatives:
        errors.append("No positive-duration water intervals remain after setup removal.")

    stream.sort()
    for (a, sub_a), (b, sub_b) in zip(stream, stream[1:]):
        (within_spacing if sub_a == sub_b else boundary_spacing).append((b - a) / 1000)
    cpu_rate = ((len(stream) - 1) * 1e6 / (stream[-1][0] - stream[0][0])
                if len(stream) > 1 and stream[-1][0] > stream[0][0] else None)
    recenter = [row for row in representatives if row["xz_velocity_rms"] >= 32]
    common_recenter = [row for row in recenter if row["xz_velocity_max"] - row["xz_velocity_rms"] <= 2]
    sample_shifts = Counter((round(row["sample_vel_x"]), round(row["sample_vel_z"])) for row in recenter)
    metrics = {field: distribution([row[field] for row in representatives]) for field in
               ("position_velocity_rms", "position_velocity_max", "xz_velocity_rms", "xz_velocity_max",
                "uv_velocity_rms", "uv_velocity_max", "position_moving_vertices", "uv_moving_vertices")}
    metrics["vertical_velocity_rms"] = distribution([vertical_rms(row) for row in representatives])
    summary = {
        "directory": str(directory), "executable_sha256": settings["executable_sha256"],
        "settings": settings, "course": course, "replay_result": result,
        "raw_motion_rows": len(motion), "removed_setup_rows": setup_rows, "dry_only_groups": dry_only_groups,
        "actual_motion_rows": len(actual), "rendered_frame_records": len(frames),
        "motion_frame_count_mismatches": mismatch_groups, "positive_interval_draw_groups": positive_groups,
        "complete_weight_sequences": complete, "incomplete_weight_sequences": incomplete,
        "positive_intervals_with_only_weight_one": single_sample_groups,
        "weight_patterns": dict(weight_patterns), "velocity_over_positive_intervals": metrics,
        "intervals_with_vertical_motion": sum(vertical_rms(row) > .001 for row in representatives),
        "intervals_with_uv_motion": sum(row["uv_velocity_rms"] > .001 for row in representatives),
        "grid_recenter_intervals_xz_rms_ge_32": len(recenter),
        "recenter_intervals_with_nearly_common_xz_motion": len(common_recenter),
        "recenter_sample_xz_shifts": [{"x": x, "z": z, "count": count} for (x, z), count in sample_shifts.most_common(12)],
        "cpu_render_preparation_record_rate_hz": cpu_rate,
        "cpu_spacing_within_submission_ms": distribution(within_spacing),
        "cpu_spacing_between_submissions_ms": distribution(boundary_spacing),
        "gpu_workload_ms": distribution([row["gpu_ms"] for row in frames]),
        "errors": errors,
    }
    return summary, {interval_key(row): row for row in representatives}


def compare(before, after, old_intervals, new_intervals):
    comparable_fields = ("quality", "style", "ripples", "fps", "debug", "height", "msaa", "course", "players", "mode", "script_sha256", "profiles_sha256")
    differences = {field: [before["settings"].get(field), after["settings"].get(field)]
                   for field in comparable_fields if before["settings"].get(field) != after["settings"].get(field)}
    keys = sorted(old_intervals.keys() & new_intervals.keys())
    recenter_keys = [key for key in keys if old_intervals[key]["xz_velocity_rms"] >= 32]
    metrics = {}
    for field in ("xz_velocity_rms", "xz_velocity_max", "uv_velocity_rms", "uv_velocity_max"):
        metrics[field] = {"before": distribution([old_intervals[key][field] for key in keys]),
                          "after": distribution([new_intervals[key][field] for key in keys])}
    return {"setting_differences": differences, "common_positive_interval_draw_groups": len(keys),
            "before_only_intervals": len(old_intervals.keys() - new_intervals.keys()),
            "after_only_intervals": len(new_intervals.keys() - old_intervals.keys()),
            "common_metrics": metrics, "before_recenter_common_intervals": len(recenter_keys),
            "after_recenter_intervals_with_vertical_motion": sum(vertical_rms(new_intervals[key]) > .001 for key in recenter_keys),
            "after_recenter_intervals_with_uv_motion": sum(new_intervals[key]["uv_velocity_rms"] > .001 for key in recenter_keys)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", type=Path, nargs="+", help="one run, or BEFORE AFTER runs")
    parser.add_argument("--course", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--require-spatial-match", action="store_true", help="require final run to have XZ max <=0.001 and retain nonzero vertical AND UV motion in positive intervals")
    args = parser.parse_args()
    if len(args.runs) not in (1, 2):
        parser.error("provide one run, or a before/after pair")
    report = {"schema_version": 1, "limitations": [
        "Velocities are per-game-interval deltas, not units per second. Statistics use one sample per draw and positive interval.",
        "CPU timestamps show batched draw preparation, not GPU completion, swapchain pacing, or displayed FPS.",
        "Incomplete weight sequences and GPU timing changes are reported, not automatic visual or performance failures.",
        "Zero XZ motion alone can also mean rejected history. Nonzero vertical and UV motion guards only against globally disabled interpolation.",
        "This report does not establish visual quality, correct interpolation at every vertex, or unchanged gameplay; use native captures and gameplay traces.",
    ], "runs": []}
    intervals = []
    for directory in args.runs:
        summary, rows = summarize(directory, args.course)
        report["runs"].append(summary)
        intervals.append(rows)
    failures = [f"{run['directory']}: {error}" for run in report["runs"] for error in run["errors"]]
    if len(args.runs) == 2:
        report["comparison"] = compare(*report["runs"], *intervals)
        if report["comparison"]["setting_differences"]:
            failures.append("Before/after render or fixture settings differ; comparison is not controlled.")
        if not report["comparison"]["common_positive_interval_draw_groups"]:
            failures.append("Before/after runs have no common positive water intervals.")
    if args.require_spatial_match:
        final = report["runs"][-1]
        maximum = final["velocity_over_positive_intervals"]["xz_velocity_max"]["max"]
        if maximum is None or maximum > .001:
            failures.append("Final trace still has XZ vertex deltas greater than 0.001; spatially fixed water motion was required.")
        if not final["intervals_with_vertical_motion"] or not final["intervals_with_uv_motion"]:
            failures.append("Final trace lacks nonzero vertical or UV motion; zero XZ alone does not demonstrate working interpolation.")
    report["failures"] = failures
    report["passed"] = not failures
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": report["passed"], "output": str(args.output), "failures": failures}))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
