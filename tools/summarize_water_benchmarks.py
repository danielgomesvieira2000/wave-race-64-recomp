#!/usr/bin/env python3
"""Summarize comparable, bounded native water replay timing runs.

Only uninstrumented runs with the same script, presentation rate, output,
course and player count are compared. GPU times cover the game rendering
workload, including water integration, but exclude the separate VI presentation
and UI queue. These are rendering costs, not end-to-end display latency.
"""
import argparse
import csv
import json
from pathlib import Path
import statistics


def percentile(values, fraction):
    values = sorted(values)
    at = (len(values) - 1) * fraction
    lo = int(at)
    return values[lo] + (values[min(lo + 1, len(values) - 1)] - values[lo]) * (at - lo)


def summarize(directory, start, end):
    settings = json.loads((directory / "settings.json").read_text())
    result = json.loads((directory / "result.json").read_text())
    if not result.get("passed"):
        raise ValueError(f"{directory}: no clean completed run")
    if any(settings.get(k) for k in ("stats", "pass_profile", "motion_trace", "debug", "shader_failure", "compare_at")):
        raise ValueError(f"{directory}: diagnostic or comparison mode enabled")
    if settings.get("saved_appearance"):
        raise ValueError(f"{directory}: benchmark appearance must be an explicit process-local override")
    rows = list(csv.DictReader((directory / "frames.csv").open()))
    samples = [row for row in rows if start <= int(row["submission"]) <= end and int(row["width"]) > 0]
    if len(samples) < 30:
        raise ValueError(f"{directory}: insufficient frames in the requested window")
    expected = {"original": 0, "modern": 1, "high": 2}[settings["quality"]]
    if any(int(row["quality"]) != expected for row in samples):
        raise ValueError(f"{directory}: active quality does not match requested quality")
    if expected and sum(int(row["water_views"]) >= settings["players"] for row in samples) < len(samples) * .95:
        raise ValueError(f"{directory}: a player view is missing replacement water in the measured race window")
    extents = sorted({(int(row["width"]), int(row["height"])) for row in samples})
    msaa = sorted({int(row["msaa"]) for row in samples})
    report = {"directory": str(directory), "quality": settings["quality"], "frames": len(samples),
              "style": settings.get("style", "modern") if expected else "original",
              "ripples": settings.get("ripples", "normal") if expected else None,
              "spray": settings.get("spray", "on") if expected else None,
              "active_extents": extents, "msaa": msaa, "output_height": settings.get("height"),
              "water_view_counts": sorted({int(row["water_views"]) for row in samples}),
              "players": settings["players"], "course": settings.get("course"),
              "fps": settings["fps"], "mode": "versus" if settings["players"] == 2 else settings["mode"],
              "peak_process_rss_mib": result["peak_process_rss_kib"] / 1024,
              "water_payload_mib": max(int(row["water_allocation_bytes"]) for row in samples) / 2**20}
    for field in ("cpu_prepare_ms", "gpu_ms"):
        values = [float(row[field]) for row in samples]
        report[field] = {"median": statistics.median(values), "p95": percentile(values, .95), "p99": percentile(values, .99)}
    # Include the first replacement frame separately instead of hiding its
    # allocation and pipeline compilation in a warm-frame percentile.
    first = next((row for row in rows if int(row["water_views"]) > 0), None)
    if first:
        report["first_water_frame"] = {field: float(first[field]) for field in ("cpu_prepare_ms", "gpu_ms")}
    key = tuple(settings.get(field) for field in ("fps", "height", "msaa", "course", "players", "mode", "script_sha256", "executable_sha256", "profiles_sha256", "spray"))
    return key, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", nargs="+", type=Path)
    parser.add_argument("--from-submission", type=int, default=1190)
    parser.add_argument("--through-submission", type=int, default=1390)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    groups = {}
    reports = []
    for directory in args.runs:
        key, report = summarize(directory, args.from_submission, args.through_submission)
        groups.setdefault(key, []).append(report)
        reports.append(report)
    for group in groups.values():
        originals = [report for report in group if report["quality"] == "original"]
        if len(originals) != 1:
            raise ValueError("Each comparison group must contain exactly one matching Original run")
        baseline = originals[0]
        for report in group:
            if report["active_extents"] != baseline["active_extents"] or report["msaa"] != baseline["msaa"]:
                raise ValueError("Comparison targets have different actual render extents or MSAA")
            report["baseline"] = baseline["directory"]
            report["median_gpu_delta_ms"] = report["gpu_ms"]["median"] - baseline["gpu_ms"]["median"]
            report["median_cpu_delta_ms"] = report["cpu_prepare_ms"]["median"] - baseline["cpu_prepare_ms"]["median"]
    args.output.write_text(json.dumps({"submission_window": [args.from_submission, args.through_submission], "runs": reports}, indent=2) + "\n")
    print(f"Wrote {len(reports)} measured runs to {args.output}")


if __name__ == "__main__":
    main()
