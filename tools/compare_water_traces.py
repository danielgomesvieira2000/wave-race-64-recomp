#!/usr/bin/env python3
"""Compare ordered game-task checkpoints, including missing/extra checkpoints.

This is a physics/render-regression gate, not an image-quality test. Supply
traces from the same ROM, fixed seed, tick script and build. An explicit end
tick avoids comparing interactive behavior after a fixture ends.
"""
import argparse
import csv
import json
from pathlib import Path


def compare(reference, candidate, through):
    def read(path):
        with path.open(newline="") as stream:
            return [row for row in csv.DictReader(stream) if int(row["tick"]) <= through]
    a, b = read(reference), read(candidate)
    differences = []
    for index, (left, right) in enumerate(zip(a, b)):
        fields = {key: [left.get(key), right.get(key)] for key in left.keys() | right.keys()
                  if left.get(key) != right.get(key)}
        if fields:
            differences.append({"checkpoint": index, "tick": left["tick"], "fields": fields})
    return {"reference": str(reference), "candidate": str(candidate), "through_tick": through,
            "reference_count": len(a), "candidate_count": len(b),
            "passed": bool(a) and len(a) == len(b) and not differences,
            "mismatch_count": len(differences), "first_mismatches": differences[:5]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--through", type=int, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = compare(args.reference, args.candidate, args.through)
    text = json.dumps(result, indent=2)
    print(text)
    if args.output:
        args.output.write_text(text + "\n")
    raise SystemExit(0 if result["passed"] else 1)
