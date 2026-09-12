#!/usr/bin/env python3
"""Make a built .app self-contained: bundle its dylibs and ad-hoc sign it.

A freshly linked bundle is not portable. It refers to SDL2 and FreeType by the
absolute paths they had on the build machine -- `/opt/homebrew/lib/...` -- so on
any machine without Homebrew, or with a different Homebrew prefix, it fails to
launch with "Library not loaded" and nothing more. And it is unsigned, which on
Apple Silicon means it will not run at all: arm64 binaries must carry at least
an ad-hoc signature.

So this walks the dependency graph from the executable, copies every non-system
library into Contents/Frameworks, rewrites the references to point there, and
signs the result from the leaves inward -- signing an outer binary first would
invalidate its signature as soon as an inner one was rewritten.

`LSMinimumSystemVersion` is derived from the `minos` of every Mach-O actually
shipped, not from the SDK the build used. A bundled Homebrew library can require
a newer OS than the app itself was compiled for, and the bundle is only as
portable as its least portable part.

Ad-hoc signed, not notarized: it runs locally and for anyone who clears it in
Gatekeeper, and it is not a distributable signature.

Run by tools/build_macos.sh; by hand:
    python3 tools/package_macos.py build-macos/WaveRace64Recomp.app
"""

import plistlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

SYSTEM_PREFIXES = ("/usr/lib/", "/System/Library/")
BUNDLED_PREFIX = "@executable_path/../Frameworks/"


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def dependencies(binary: Path):
    """The install names a Mach-O file asks the loader for."""
    out = subprocess.check_output(["otool", "-L", str(binary)], text=True)
    # The first line is the file's own name, not a dependency.
    return [line.strip().split(" (", 1)[0] for line in out.splitlines()[1:]]


def minimum_os(binary: Path):
    out = subprocess.check_output(["otool", "-l", str(binary)], text=True)
    return re.findall(r"^\s*minos (\d+(?:\.\d+){0,2})$", out, re.MULTILINE)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} <path to WaveRace64Recomp.app>")

    app = Path(sys.argv[1]).resolve()
    executable = app / "Contents" / "MacOS" / "WaveRace64Recomp"
    if not executable.is_file():
        raise SystemExit(f"No built application at {app}")

    assets = app / "Contents" / "Resources" / "assets"
    if not (assets / "recomp.rcss").is_file():
        raise SystemExit(
            f"The frontend's assets are missing from {assets}.\n"
            "They are staged by the build; build the WaveRace64Recomp target first.")

    frameworks = app / "Contents" / "Frameworks"
    frameworks.mkdir(parents=True, exist_ok=True)

    # Breadth of the graph, not just the executable's direct dependencies:
    # SDL2 pulls in its own, and each of those has to be rewritten too.
    pending = [executable]
    seen = set()
    while pending:
        binary = pending.pop()
        if binary in seen:
            continue
        seen.add(binary)

        for dep in dependencies(binary):
            if dep.startswith(SYSTEM_PREFIXES):
                continue
            if dep.startswith(BUNDLED_PREFIX):
                # Already rewritten, by an earlier pass or a previous run.
                pending.append(frameworks / Path(dep).name)
                continue
            if not dep.startswith("/"):
                raise SystemExit(
                    f"{binary.name} asks for {dep!r}, which is neither absolute nor\n"
                    "already bundled, so there is no way to know what to copy.")

            source = Path(dep)
            dest = frameworks / source.name
            # Copied every time rather than only when absent: a rebuild may have
            # produced a different library of the same name, and silently
            # shipping the older one is the kind of bug that only appears on
            # someone else's machine.
            shutil.copy2(source.resolve(), dest)
            dest.chmod(0o755)
            run("install_name_tool", "-id", BUNDLED_PREFIX + dest.name, dest)
            run("install_name_tool", "-change", dep, BUNDLED_PREFIX + dest.name, binary)
            pending.append(dest)

    print(f"bundled {len(seen) - 1} librar{'y' if len(seen) == 2 else 'ies'}")

    info_path = app / "Contents" / "Info.plist"
    info = plistlib.loads(info_path.read_bytes())
    info["NSHighResolutionCapable"] = True

    versions = [v for binary in seen for v in minimum_os(binary)]
    if not versions:
        raise SystemExit("No macOS deployment target found in any binary in the bundle.")
    info["LSMinimumSystemVersion"] = max(
        versions, key=lambda v: tuple(int(part) for part in v.split(".")))
    info_path.write_bytes(plistlib.dumps(info))
    print(f"minimum macOS: {info['LSMinimumSystemVersion']}")

    # Inside out. Signing the bundle first and then rewriting a library in it
    # leaves a signature that no longer matches its contents.
    for binary in sorted(seen):
        if binary != executable:
            run("codesign", "--force", "--sign", "-", binary)
    run("codesign", "--force", "--sign", "-", app)
    run("codesign", "--verify", "--deep", "--strict", app)

    print(f"packaged and verified {app}")


if __name__ == "__main__":
    main()
