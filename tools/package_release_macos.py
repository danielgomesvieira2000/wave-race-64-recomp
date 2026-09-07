#!/usr/bin/env python3
"""Create a signed-app release ZIP, notices, provenance, and checksum."""

import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def run(*args, **kwargs):
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, default=ROOT / "build-macos-release/WaveRace64Recomp.app")
    parser.add_argument("--version", required=True, help="Release suffix, for example 0.4.0-macos.1")
    parser.add_argument("--output", type=Path, default=ROOT / "dist")
    parser.add_argument("--dependency-prefix", type=Path, required=True)
    parser.add_argument("--dependency-manifest", type=Path, required=True)
    parser.add_argument("--minimum-macos", default="15.0", help="Maximum permitted deployment requirement")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", args.version):
        parser.error("Version must contain only letters, numbers, dots, underscores, or hyphens")
    app = args.app.resolve()
    run("codesign", "--verify", "--deep", "--strict", app)
    run("git", "diff", "HEAD", "--quiet", "--ignore-submodules=dirty", cwd=ROOT)
    name = f"WaveRace64Recomp-{args.version}-apple-silicon"
    output = args.output.resolve()
    stage = output / name
    archive = output / f"{name}.zip"
    if stage.exists() or archive.exists():
        parser.error(f"Release output already exists: {stage}")
    stage.mkdir(parents=True)
    run("ditto", "--norsrc", "--noextattr", "--noqtn", app, stage / app.name)
    for filename in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
        shutil.copyfile(ROOT / filename, stage / filename)
    shutil.copyfile(ROOT / "docs/MACOS_RELEASE.md", stage / "START_HERE.md")

    # Preserve the actual license texts alongside the component inventory.
    license_paths = (
        "lib/N64ModernRuntime/COPYING", "lib/N64ModernRuntime/N64Recomp/LICENSE",
        "lib/N64ModernRuntime/thirdparty/o1heap/LICENSE", "lib/N64ModernRuntime/thirdparty/miniz/LICENSE",
        "lib/N64ModernRuntime/thirdparty/xxHash/LICENSE", "lib/RT64/LICENSE",
        "lib/RT64/src/contrib/plume/LICENSE", "lib/RT64/src/contrib/plume/contrib/metal-cpp/LICENSE.txt",
        "lib/RT64/src/contrib/hlslpp/LICENSE", "lib/RT64/src/contrib/imgui/LICENSE.txt",
        "lib/RT64/src/contrib/implot/LICENSE", "lib/RT64/src/contrib/im3d/LICENSE",
        "lib/RT64/src/contrib/nativefiledialog-extended/LICENSE", "lib/RT64/src/contrib/xxHash/LICENSE",
        "lib/RT64/src/contrib/zstd/LICENSE", "lib/RT64/src/contrib/stb/LICENSE",
        "lib/RT64/src/contrib/ddspp/LICENSE", "lib/RT64/src/contrib/re-spirv/LICENSE",
        "lib/RT64/src/contrib/spirv-cross/LICENSE", "lib/RecompFrontend/lib/GamepadMotionHelpers/LICENSE",
        "lib/RecompFrontend/recompui/lib/RmlUi/LICENSE.txt",
        "lib/RecompFrontend/recompui/lib/RmlUi/Samples/assets/LICENSE.txt",
        "lib/RecompFrontend/recompui/lib/lunasvg/LICENSE",
        "lib/RecompFrontend/recompui/lib/lunasvg/plutovg/LICENSE", "assets/promptfont/LICENSE.txt",
    )
    for filename in license_paths:
        dest = stage / "licenses" / filename
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / filename, dest)
    shutil.copytree(args.dependency_prefix / "share/wr64-licenses", stage / "licenses/macos-libraries")
    shutil.copyfile(args.dependency_manifest, stage / "macos-dependencies.json")

    executable = stage / app.name / "Contents/MacOS/WaveRace64Recomp"
    with (stage / app.name / "Contents/Info.plist").open("rb") as stream:
        info = plistlib.load(stream)
    def version(value):
        parts = tuple(map(int, value.split(".")))
        return parts + (0,) * (3 - len(parts))
    if version(info["LSMinimumSystemVersion"]) > version(args.minimum_macos):
        raise RuntimeError("Bundle requires a newer macOS than the release permits; rebuild its libraries")
    architecture = subprocess.check_output(["lipo", "-archs", executable], text=True).strip()
    if architecture != "arm64":
        raise RuntimeError(f"Expected an arm64 release executable, found {architecture}")
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    submodules = subprocess.check_output(["git", "submodule", "status", "--recursive"], cwd=ROOT, text=True)
    manifest = {
        "release": args.version,
        "source_commit": revision,
        "source_url": f"https://github.com/elliotttate/wave-race-64-recomp/tree/{revision}",
        "minimum_macos": info["LSMinimumSystemVersion"],
        "architecture": architecture,
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "signing": "ad-hoc; not notarized",
        "submodules": submodules.splitlines(),
        "patch_instructions": "Apply tools/build_macos.sh patches before building; see docs/MACOS.md.",
    }
    (stage / "BUILD.json").write_text(json.dumps(manifest, indent=2) + "\n")
    forbidden = {".z64", ".n64", ".v64", ".rom", ".bin", ".eep", ".sra", ".fla", ".log"}
    for item in stage.rglob("*"):
        if item.suffix.lower() in forbidden or item.name in ("portable.txt", ".DS_Store"):
            raise RuntimeError(f"Unexpected game/user data in release: {item}")
    run("codesign", "--verify", "--deep", "--strict", stage / app.name)
    run("ditto", "-c", "-k", "--norsrc", "--noextattr", "--noqtn", "--keepParent", stage, archive)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    checksum = output / f"{name}.zip.sha256"
    checksum.write_text(f"{digest}  {archive.name}\n")
    print(json.dumps({"archive": str(archive), "checksum": str(checksum), **manifest}, indent=2))


if __name__ == "__main__":
    main()
