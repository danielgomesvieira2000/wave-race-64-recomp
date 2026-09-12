#!/usr/bin/env python3
"""Stage a built Linux or macOS tree into a release archive.

The Windows equivalent is tools/package_release.ps1; this is the same idea for
the other two, and deliberately makes the same promises.

What goes in: the program, the assets the menus draw from, this project's
LICENSE, the third-party notices and the README. On macOS that is the signed
.app bundle, which already carries its own libraries (see
tools/package_macos.py). On Linux it is the executable plus a launcher, and the
libraries it needs are the distribution's -- SDL2, Vulkan, GTK3, FreeType --
which is why the archive says so rather than shipping copies that would go
stale.

What it is not: a ROM. The program contains the game's *code*, statically
recompiled into C and compiled into the binary, and none of its assets, which
are read from the player's own dump every time it runs. The script refuses to
continue if it finds a dump or a save anywhere in the staging directory, and it
refuses to overwrite an archive that already exists -- a published checksum
should never quietly start describing different bytes.

Whether to distribute such a binary at all is a decision for the project's
owner and not for this script. See the README's Licensing section first.

    python3 tools/package_release.py --version 0.8.1
    python3 tools/package_release.py --version 0.8.1 --build-dir build-linux
"""

import argparse
import hashlib
import platform
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Anything that could be part of someone's copy of the game, or their progress
# in it. None of it belongs in a release archive.
FORBIDDEN_SUFFIXES = {".z64", ".n64", ".v64", ".rom", ".bin",
                      ".eep", ".sra", ".fla", ".mpk", ".srm"}

DOCS = ["LICENSE", "THIRD_PARTY_NOTICES.md", "README.md"]

LINUX_LAUNCHER = """#!/usr/bin/env bash
# Wave Race 64: Recompiled
#
# The libraries this needs come from your distribution, not from this archive:
#
#   Debian, Ubuntu   libsdl2-2.0-0 libvulkan1 libgtk-3-0 libfreetype6
#                    mesa-vulkan-drivers (or your GPU vendor's Vulkan driver)
#   Fedora           SDL2 vulkan-loader gtk3 freetype mesa-vulkan-drivers
#   Arch             sdl2 vulkan-icd-loader gtk3 freetype2 + a vulkan driver
#
# Settings, saves and mods live in $XDG_DATA_HOME/WaveRace64Recomp, or
# ~/.local/share/WaveRace64Recomp. Put a file called portable.txt next to this
# script to keep them here instead.
set -euo pipefail
cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"
exec ./WaveRace64Recomp "$@"
"""

LINUX_NOTES = """Wave Race 64: Recompiled -- Linux x86-64
=======================================

Run ./WaveRace64Recomp.sh, and pick your own Wave Race 64 (USA) (Rev A) dump in
the launcher. No dump is included here and none ever will be; see README.md.

Runtime dependencies, from your distribution:

    Debian, Ubuntu   libsdl2-2.0-0 libvulkan1 libgtk-3-0 libfreetype6
                     mesa-vulkan-drivers, or your GPU vendor's Vulkan driver
    Fedora           SDL2 vulkan-loader gtk3 freetype mesa-vulkan-drivers
    Arch             sdl2 vulkan-icd-loader gtk3 freetype2 + a Vulkan driver

RT64 renders through Vulkan here, so a working Vulkan driver is not optional.
`vulkaninfo --summary` should name your GPU. If it does not, the port will
report that it could not set up the renderer.

Settings, saves and mods live in $XDG_DATA_HOME/WaveRace64Recomp, or
~/.local/share/WaveRace64Recomp. A file called portable.txt beside the
executable keeps them next to it instead.
"""


def fail(message):
    raise SystemExit(f"error: {message}")


def check_no_game_data(stage: Path):
    found = [p for p in stage.rglob("*")
             if p.is_file() and p.suffix.lower() in FORBIDDEN_SUFFIXES]
    if found:
        for p in found:
            print(f"refusing to package {p}", file=sys.stderr)
        fail("game data found in the staging directory")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def split_debug_info(binary: Path, symbols: Path) -> bool:
    """Move the debug info out of the binary, if a suitable objcopy exists.

    RelWithDebInfo leaves DWARF inside the executable, which multiplies its size
    several times over for something only a crash report needs.
    """
    objcopy = shutil.which("llvm-objcopy") or shutil.which("objcopy")
    if objcopy is None:
        return False
    subprocess.run([objcopy, "--only-keep-debug", str(binary), str(symbols)], check=True)
    subprocess.run([objcopy, "--strip-debug",
                    f"--add-gnu-debuglink={symbols}", str(binary)], check=True)
    return True


def stage_linux(build_dir: Path, stage: Path) -> Path:
    binary = build_dir / "WaveRace64Recomp"
    if not binary.is_file():
        fail(f"no executable at {binary} -- build first (see docs/BUILDING.md)")
    assets = build_dir / "assets"
    if not (assets / "recomp.rcss").is_file():
        fail(f"the frontend's assets are missing from {assets}")

    shutil.copy2(binary, stage / binary.name)
    (stage / binary.name).chmod(0o755)
    shutil.copytree(assets, stage / "assets")

    launcher = stage / "WaveRace64Recomp.sh"
    launcher.write_text(LINUX_LAUNCHER, encoding="utf-8")
    launcher.chmod(0o755)
    (stage / "README-LINUX.txt").write_text(LINUX_NOTES, encoding="utf-8")
    return stage / binary.name


def stage_macos(build_dir: Path, stage: Path):
    app = build_dir / "WaveRace64Recomp.app"
    if not app.is_dir():
        fail(f"no application at {app} -- build first (see docs/BUILDING.md)")
    if not (app / "Contents" / "Frameworks").is_dir():
        fail(f"{app.name} has not been bundled. Run: python3 tools/package_macos.py {app}")
    # copytree with symlinks=True: a framework's Versions/Current is a symlink,
    # and following it both duplicates the payload and breaks the signature.
    shutil.copytree(app, stage / app.name, symlinks=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--version", required=True, help="e.g. 0.8.1")
    parser.add_argument("--build-dir", type=Path, default=None,
                        help="default: build-linux or build-macos")
    parser.add_argument("--out-dir", type=Path, default=ROOT / "dist")
    args = parser.parse_args()

    system = platform.system()
    if system == "Linux":
        target, ext = f"linux-{platform.machine()}", ".tar.gz"
        default_build = "build-linux"
    elif system == "Darwin":
        target, ext = f"macos-{platform.machine()}", ".zip"
        default_build = "build-macos"
    else:
        fail(f"{system} is not handled here. On Windows use tools/package_release.ps1.")

    build_dir = (args.build_dir or ROOT / default_build).resolve()
    name = f"WaveRace64Recomp-{args.version}-{target}"
    archive = args.out_dir / (name + ext)
    symbols_archive = args.out_dir / f"{name}-debug-symbols.tar.gz"

    if archive.exists():
        fail(f"{archive} already exists. Bump --version or delete it deliberately;\n"
             "       overwriting a published archive changes what its checksum means.")

    args.out_dir.mkdir(parents=True, exist_ok=True)
    stage = args.out_dir / name
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)

    binary = None
    if system == "Linux":
        binary = stage_linux(build_dir, stage)
    else:
        stage_macos(build_dir, stage)

    for doc in DOCS:
        source = ROOT / doc
        if not source.is_file():
            fail(f"missing {doc}")
        shutil.copy2(source, stage / source.name)

    check_no_game_data(stage)

    symbols = None
    if binary is not None:
        candidate = stage / (binary.name + ".debug")
        if split_debug_info(binary, candidate):
            symbols = candidate
        else:
            print("note: no objcopy found; debug info stays in the executable")

    if symbols is not None:
        with tarfile.open(symbols_archive, "w:gz") as tar:
            tar.add(symbols, arcname=f"{name}/{symbols.name}")
        symbols.unlink()
        print(f"wrote {symbols_archive.relative_to(ROOT)}")

    if ext == ".tar.gz":
        with tarfile.open(archive, "w:gz") as tar:
            tar.add(stage, arcname=name)
    else:
        # ZIP, not tar, because that is what macOS unpacks by double-click --
        # and written with the external attributes preserved so the executable
        # bit and the symlinks inside the bundle survive.
        subprocess.run(["ditto", "-c", "-k", "--sequesterRsrc", "--keepParent",
                        str(stage), str(archive)], check=True)

    print(f"wrote {archive.relative_to(ROOT)}")
    print(f"sha256 {sha256(archive)}")
    print(f"staged tree left in {stage.relative_to(ROOT)} -- test it before publishing")


if __name__ == "__main__":
    main()
