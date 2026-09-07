#!/usr/bin/env python3
"""Compile every water shader variant to SPIR-V and DXIL using bundled DXC.

This checks shared source/ABI compatibility, not runtime behavior on an absent
GPU backend. The normal macOS build additionally translates SPIR-V to Metal.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess

ROOT = Path(__file__).resolve().parent.parent
VARIANTS = [("WaterVS", "vs", None), ("WaterPS", "ps", None),
            ("WaterPS", "ps", "CLASSIC_STYLE"), ("WaterPS", "ps", "CLASSIC_MSAA"),
            ("WaterSurfacePS", "ps", None), ("WaterSprayVS", "vs", None),
            ("WaterSprayPS", "ps", None), ("WaterSprayPS", "ps", "SPRAY_MSAA"),
            ("WaterNormalsCS", "cs", None), ("WaterCaptureCS", "cs", None),
            ("WaterCaptureCS", "cs", "CAPTURE_MSAA"), ("WaterDepthReduceCS", "cs", None),
            ("WaterOriginalCS", "cs", None), ("WaterOriginalCS", "cs", "CAPTURE_MSAA"),
            ("WaterFieldCS", "cs", None), ("WaterStatsCS", "cs", None)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/water-backends")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    source = ROOT / "lib/RT64/src"
    arch = "arm64" if platform.machine().lower() in ("arm64", "aarch64") else "x64"
    system = platform.system()
    binary = "dxc.exe" if system == "Windows" else "dxc-macos" if system == "Darwin" else "dxc-linux"
    dxc = source / "contrib/dxc/bin" / arch / binary
    env = os.environ.copy()
    if system != "Windows":
        variable = "DYLD_LIBRARY_PATH" if system == "Darwin" else "LD_LIBRARY_PATH"
        env[variable] = str(source / "contrib/dxc/lib" / arch) + os.pathsep + env.get(variable, "")
    results = []
    for name, stage, define in VARIANTS:
        shader = source / "shaders" / (name + ".hlsl")
        label = name + ("-" + define if define else "")
        for backend in ("spirv", "dxil"):
            command = [str(dxc), str(shader), "-I", str(source), "-E", stage.upper() + "Main", "-T", stage + "_6_3"]
            if define:
                command += ["-D", define]
                if define == "CLASSIC_MSAA":
                    command += ["-D", "CLASSIC_STYLE"]
            if backend == "spirv":
                command += ["-spirv", "-fspv-target-env=vulkan1.0", "-fvk-use-dx-layout"]
                if stage == "vs":
                    command += ["-fvk-invert-y"]
            else:
                command += ["-Wno-ignored-attributes"]
            command += ["-Fo", str(args.output / (label + "." + backend))]
            completed = subprocess.run(command, env=env, capture_output=True, text=True)
            (args.output / (label + "." + backend + ".log")).write_text(completed.stdout + completed.stderr)
            results.append({"shader": label, "backend": backend, "passed": completed.returncode == 0,
                            "shared_params_sha256": hashlib.sha256((source / "shared/rt64_water_params.h").read_bytes()).hexdigest(),
                            "source_sha256": hashlib.sha256(shader.read_bytes()).hexdigest()})
    (args.output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    passed = sum(row["passed"] for row in results)
    print(f"{passed}/{len(results)} shader compile checks passed; runtime validation is separate")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
