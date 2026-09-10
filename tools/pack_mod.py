"""Zip a mod directory into the `.nrm` the game installs.

A mod is a zip with a `mod.json` at its root and whatever content it
carries; the extension is what librecomp scans for. There is nothing else to it,
which is why this is fifty lines rather than a build system.

    python tools/pack_mod.py examples/mods/texture-pack-template
    python tools/pack_mod.py <dir> --out dist/mods
    python tools/pack_mod.py <dir> --install     # straight into the mods folder

`--install` writes it where the game scans, which is
`%LOCALAPPDATA%\\WaveRace64Recomp\\mods` on Windows. The Mods tab's refresh
button picks it up without restarting.

The name comes from the manifest's id and version, so two builds of the same mod
do not sit in the folder as separate mods.

**Nothing from the cartridge belongs in one of these.** A texture pack replaces
the game's textures with your own images; the game's own textures, and anything
derived from them by upscaling or filtering, are its data and stay out. See
CONTRIBUTING.md.
"""

import argparse
import json
import os
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def settings_directory():
    """The same directory wr64::settings_directory() picks (src/main.cpp)."""
    if Path("portable.txt").exists():
        return Path.cwd()
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA")
        if base:
            return Path(base) / "WaveRace64Recomp"
    else:
        base = os.environ.get("XDG_DATA_HOME")
        if base:
            return Path(base) / "WaveRace64Recomp"
        home = os.environ.get("HOME")
        if home:
            return Path(home) / ".local" / "share" / "WaveRace64Recomp"
    return Path.cwd()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("directory", type=Path, help="the mod's directory, holding mod.json")
    ap.add_argument("--out", type=Path, default=REPO / "dist" / "mods",
                    help="where to write the .nrm (default dist/mods)")
    ap.add_argument("--install", action="store_true",
                    help="write it into the game's mods folder instead")
    args = ap.parse_args()

    source = args.directory
    manifest_path = source / "mod.json"
    if not manifest_path.is_file():
        sys.exit(f"no mod.json in {source}")

    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except ValueError as e:
        sys.exit(f"{manifest_path} is not valid JSON: {e}")

    for field in ("game_id", "id", "display_name", "version", "authors",
                  "minimum_recomp_version"):
        if field not in manifest:
            sys.exit(f"{manifest_path} is missing the required field {field!r}")

    out_dir = settings_directory() / "mods" if args.install else args.out
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / f"{manifest['id']}-{manifest['version']}.nrm"

    files = sorted(p for p in source.rglob("*") if p.is_file())
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for path in files:
            zf.write(path, path.relative_to(source).as_posix())

    print(f"  {out_path}  ({out_path.stat().st_size} bytes, {len(files)} file(s))")
    for path in files:
        print(f"    {path.relative_to(source).as_posix()}")
    if args.install:
        print("\nOpen the Mods tab and press refresh, or restart the game.")


if __name__ == "__main__":
    main()
