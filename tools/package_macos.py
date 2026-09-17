#!/usr/bin/env python3
"""Make a built .app self-contained: bundle its dylibs and ad-hoc sign it.

A freshly linked bundle is not portable. It refers to SDL2 and FreeType by the
absolute paths they had on the build machine -- `/opt/homebrew/lib/...` -- so on
any machine without Homebrew, or with a different Homebrew prefix, it fails to
launch with "Library not loaded" and nothing more. An SDL2.framework linked as
`@rpath/SDL2.framework/...` is worse: if the executable has no LC_RPATH, dyld
aborts before main. And it is unsigned, which on Apple Silicon means it will
not run at all: arm64 binaries must carry at least an ad-hoc signature.

So this walks the dependency graph from the executable, copies every non-system
library or framework into Contents/Frameworks, rewrites the references to point
there, and signs the result from the leaves inward -- signing an outer binary
first would invalidate its signature as soon as an inner one was rewritten.

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

SYSTEM_PREFIXES = ("/usr/lib/", "/System/Library/", "/Library/Apple/")
BUNDLED_PREFIX = "@executable_path/../Frameworks/"
FRAMEWORK_SEARCH = (
    Path.home() / "Library/Frameworks",
    Path("/Library/Frameworks"),
    Path("/Network/Library/Frameworks"),
    Path("/opt/homebrew/lib"),
    Path("/usr/local/lib"),
)


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def dependencies(binary: Path):
    """The install names a Mach-O file asks the loader for."""
    out = subprocess.check_output(["otool", "-L", str(binary)], text=True)
    names = []
    # Skip the file's own name. Universal binaries print a header per arch
    # ("SDL2 (architecture arm64):") which is not an install name. A dylib's
    # first remaining entry is usually its own LC_ID_DYLIB; the caller skips
    # a dep that resolves to the file being walked.
    for line in out.splitlines()[1:]:
        name = line.strip().split(" (", 1)[0]
        if not name or name.endswith(":"):
            continue
        if name.startswith("@") or name.startswith("/"):
            names.append(name)
    return names


def rpaths(binary: Path):
    """LC_RPATH entries recorded in a Mach-O file."""
    out = subprocess.check_output(["otool", "-l", str(binary)], text=True)
    paths = []
    lines = out.splitlines()
    for i, line in enumerate(lines):
        if "cmd LC_RPATH" not in line:
            continue
        for follow in lines[i + 1:i + 8]:
            m = re.match(r"\s*path\s+(\S+)", follow)
            if m:
                paths.append(m.group(1))
                break
    return paths


def minimum_os(binary: Path):
    out = subprocess.check_output(["otool", "-l", str(binary)], text=True)
    return re.findall(r"^\s*minos (\d+(?:\.\d+){0,2})$", out, re.MULTILINE)


def expand_load_token(token: str, binary: Path, executable: Path) -> str:
    token = token.replace("@executable_path", str(executable.parent))
    token = token.replace("@loader_path", str(binary.parent))
    return token


def framework_root(path: Path):
    for candidate in (path, *path.parents):
        if candidate.suffix == ".framework":
            return candidate
    return None


def resolve_dep(binary: Path, dep: str, executable: Path):
    """Turn an install name into a file on this machine, or None."""
    if dep.startswith("/"):
        path = Path(dep)
        return path if path.exists() else None

    rest = None
    if dep.startswith("@rpath/"):
        rest = dep[len("@rpath/"):]
    elif dep.startswith("@loader_path/") or dep.startswith("@executable_path/"):
        expanded = Path(expand_load_token(dep, binary, executable))
        return expanded if expanded.exists() else None
    else:
        return None

    search = [Path(expand_load_token(p, binary, executable)) for p in rpaths(binary)]
    search.extend(FRAMEWORK_SEARCH)
    for root in search:
        candidate = root / rest
        if candidate.exists():
            return candidate
    return None


def bundle_source(source: Path, frameworks: Path):
    """Copy a dylib or a whole .framework into Contents/Frameworks.

    Returns (bundled Mach-O, install name to use inside the app).
    """
    fw = framework_root(source)
    if fw is not None:
        dest_fw = frameworks / fw.name
        if dest_fw.resolve() != fw.resolve():
            if dest_fw.exists():
                shutil.rmtree(dest_fw)
            shutil.copytree(fw, dest_fw, symlinks=True)
        rel = source.resolve().relative_to(fw.resolve()).as_posix()
        dest = dest_fw / rel
        dest.chmod(dest.stat().st_mode | 0o111)
        name = BUNDLED_PREFIX + f"{fw.name}/{rel}"
        return dest, name

    dest = frameworks / source.name
    shutil.copy2(source.resolve(), dest)
    dest.chmod(0o755)
    name = BUNDLED_PREFIX + dest.name
    return dest, name


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
                bundled = frameworks / dep[len(BUNDLED_PREFIX):]
                if bundled.exists():
                    pending.append(bundled)
                continue

            source = resolve_dep(binary, dep, executable)
            if source is None:
                raise SystemExit(
                    f"{binary.name} asks for {dep!r}, which is neither absolute nor\n"
                    "resolvable via LC_RPATH or a standard framework path, so there\n"
                    "is no way to know what to copy.")

            source = source.resolve()
            if source == binary.resolve():
                continue
            try:
                source.relative_to(frameworks.resolve())
            except ValueError:
                pass
            else:
                pending.append(source)
                continue

            dest, bundled_name = bundle_source(source, frameworks)
            run("install_name_tool", "-id", bundled_name, dest)
            run("install_name_tool", "-change", dep, bundled_name, binary)
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
