#!/usr/bin/env python3
"""Reconstruct water/runtime changes from pinned libraries in a temporary tree.

Verifies clean application, idempotence, and byte equality of every file named
by the patches. The working libraries and their unrelated edits are untouched.
"""
import argparse
import io
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = {"libraries": []}
    with tempfile.TemporaryDirectory(prefix="wr64-water-rebuild-") as temporary:
        tree = Path(temporary)
        (tree / "tools/patches").mkdir(parents=True)
        for script in ("patch_rt64.py", "patch_water.py", "patch_runtime_shutdown.py"):
            shutil.copyfile(ROOT / "tools" / script, tree / "tools" / script)
        for library, patch, script in [("RT64", "rt64-water.patch", "patch_water.py"),
                                       ("N64ModernRuntime", "runtime-shutdown.patch", "patch_runtime_shutdown.py")]:
            original = ROOT / "lib" / library
            destination = tree / "lib" / library
            destination.mkdir(parents=True)
            revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=original, text=True).strip()
            archive = subprocess.check_output(["git", "archive", revision], cwd=original)
            with tarfile.open(fileobj=io.BytesIO(archive)) as contents:
                contents.extractall(destination, filter="data")
            shutil.copyfile(ROOT / "tools/patches" / patch, tree / "tools/patches" / patch)
            if library == "RT64":
                subprocess.run([sys.executable, str(tree / "tools/patch_rt64.py")], cwd=tree, check=True, capture_output=True)
            logs = []
            for _ in range(2):
                result = subprocess.run([sys.executable, str(tree / "tools" / script)], cwd=tree,
                                        check=True, capture_output=True, text=True)
                logs.append(result.stdout.strip())
            files = re.findall(r"^\+\+\+ b/(.+)$", (tree / "tools/patches" / patch).read_text(), re.MULTILINE)
            mismatches = [name for name in files if (original / name).read_bytes() != (destination / name).read_bytes()]
            if mismatches:
                raise RuntimeError(f"{library}: reconstructed files differ: {mismatches}")
            report["libraries"].append({"library": library, "revision": revision, "files": len(files),
                                        "byte_exact": True, "application": logs})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
