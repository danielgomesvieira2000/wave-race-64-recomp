#!/usr/bin/env python3
"""Bundle non-system dylibs and ad-hoc sign the local macOS application."""

from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys

from bundled_assets import ROOT, stage as stage_bundled_assets


def run(*args):
    subprocess.run(list(map(str, args)), check=True)


def dependencies(path):
    output = subprocess.check_output(["otool", "-L", str(path)], text=True)
    return [line.strip().split(" (", 1)[0] for line in output.splitlines()[1:]]


def main():
    app = Path(sys.argv[1]).resolve()
    executable = app / "Contents/MacOS/WaveRace64Recomp"
    if not executable.is_file():
        raise SystemExit(f"No built application at {app}")
    # Refresh the reviewed replacement packs before signing. This also covers
    # local bundles built before the assets were added to CMake staging.
    stage_bundled_assets(ROOT / "assets", app / "Contents/Resources/assets")
    frameworks = app / "Contents/Frameworks"
    frameworks.mkdir(parents=True, exist_ok=True)
    pending = [executable]
    seen = set()
    while pending:
        binary = pending.pop()
        if binary in seen:
            continue
        seen.add(binary)
        for dep in dependencies(binary):
            if dep.startswith(("/usr/lib/", "/System/Library/")):
                continue
            if dep.startswith("@executable_path/../Frameworks/"):
                pending.append(frameworks / Path(dep).name)
                continue
            if not dep.startswith("/"):
                raise SystemExit(f"Unresolved dependency in {binary}: {dep}")
            source = Path(dep)
            dest = frameworks / source.name
            # A relink may select a newly rebuilt library with the same name.
            # Refresh it instead of silently retaining an older packaged dylib.
            shutil.copy2(source.resolve(), dest)
            dest.chmod(0o755)
            run("install_name_tool", "-id", f"@executable_path/../Frameworks/{dest.name}", dest)
            run("install_name_tool", "-change", dep, f"@executable_path/../Frameworks/{dest.name}", binary)
            pending.append(dest)
    info_path = app / "Contents/Info.plist"
    with info_path.open("rb") as f:
        info = plistlib.load(f)
    info["NSHighResolutionCapable"] = True
    # Derive the advertised minimum from every shipped Mach-O, rather than the
    # host SDK. A bundled Homebrew library can require a newer OS than the app.
    minimum = []
    for binary in seen:
        commands = subprocess.check_output(["otool", "-l", str(binary)], text=True)
        minimum.extend(re.findall(r"^\s*minos (\d+(?:\.\d+){0,2})$", commands, re.MULTILINE))
    if not minimum:
        raise SystemExit("No macOS deployment target found in the bundle")
    info["LSMinimumSystemVersion"] = max(minimum, key=lambda value: tuple(map(int, value.split("."))))
    with info_path.open("wb") as f:
        plistlib.dump(info, f)
    for binary in sorted(seen):
        if binary != executable:
            run("codesign", "--force", "--sign", "-", binary)
    run("codesign", "--force", "--sign", "-", app)
    run("codesign", "--verify", "--deep", "--strict", app)
    print(f"Packaged and verified {app}")


if __name__ == "__main__":
    main()
