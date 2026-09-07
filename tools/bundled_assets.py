#!/usr/bin/env python3
"""Validate or stage the required, manifest-pinned replacement music and textures.

Uses only the Python standard library, including on Windows build machines.
This checks distribution integrity; the reviewed art and runtime checks happen
before the manifests are published.
"""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import wave


ROOT = Path(__file__).resolve().parent.parent
PACK = Path("textures/nano-banana-2")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def member(root, name):
    require(isinstance(name, str) and name, f"Missing asset path in {root}")
    parts = PurePosixPath(name)
    require(not parts.is_absolute() and ".." not in parts.parts
            and "\\" not in name and ":" not in name,
            f"Asset path must be relative to its pack: {name}")
    path = root / parts
    require(path.resolve().is_relative_to(root.resolve()), f"Asset escapes its pack: {name}")
    require(path.is_file() and path.stat().st_size > 0,
            f"Required bundled asset missing or empty: {path}. Restore the tracked assets before packaging.")
    require(not path.is_symlink(), f"Bundled assets must be regular files: {path}")
    return path


def read_json(root, name):
    return json.loads(member(root, name).read_text(encoding="utf-8"))


def check_file(root, name, sha256, size=None):
    path = member(root, name)
    if size is not None:
        require(path.stat().st_size == size, f"Bundled asset size mismatch: {path}")
    require(digest(path) == sha256,
            f"Bundled asset checksum mismatch: {path}. Restore the file or regenerate its reviewed manifest.")
    return path


def check_contents(root, expected, optional=()):
    actual = {path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file()}
    extra = actual - set(expected) - set(optional)
    require(not extra, f"Unmanifested files in bundled assets {root}: {sorted(extra)[:5]}")


def validate_music(root):
    manifest = read_json(root, "manifest.json")
    require(manifest.get("schema_version") == 1, "Unsupported music manifest schema")
    tracks = manifest.get("tracks", [])
    require(bool(tracks), "Bundled music manifest contains no tracks")
    credits = member(root, manifest["credits_file"])
    loops_path = check_file(root, manifest["loops_file"], manifest["loops_sha256"])
    loops = json.loads(loops_path.read_text(encoding="utf-8"))
    expected = {"manifest.json", credits.relative_to(root).as_posix(), loops_path.relative_to(root).as_posix()}
    ids = set()
    for track in tracks:
        require(track["id"] not in ids, f"Duplicate music track: {track['id']}")
        ids.add(track["id"])
        require(track["output"] not in expected, f"Duplicate music output: {track['output']}")
        path = check_file(root, track["output"], track["output_sha256"], track["output_bytes"])
        expected.add(track["output"])
        with wave.open(str(path), "rb") as audio:
            require((audio.getnchannels(), audio.getsampwidth(), audio.getframerate()) == (2, 2, 48000),
                    f"Expected stereo 48 kHz PCM16 music: {path}")
            require(audio.getnframes() == track["output_frames"], f"Music frame count mismatch: {path}")
            duration = audio.getnframes() / audio.getframerate()
        loop = loops[track["id"]]
        require(loop == track["loop"], f"Music loop manifest mismatch: {track['id']}")
        require(0 <= loop["start_seconds"] < loop["end_seconds"] <= duration
                and 0 <= loop["crossfade_seconds"] < loop["end_seconds"] - loop["start_seconds"],
                f"Music loop is outside the recording: {track['id']}")
    require(set(loops) == ids, "Music loop/track inventory mismatch")
    check_contents(root, expected)
    return {"tracks": len(tracks), "manifest_sha256": digest(root / "manifest.json")}


def image_header(path):
    with path.open("rb") as stream:
        header = stream.read(128)
    if path.suffix == ".png":
        require(header[:8] == b"\x89PNG\r\n\x1a\n" and header[12:16] == b"IHDR",
                f"Invalid PNG header: {path}")
        width, height = struct.unpack_from(">II", header, 16)
        return width, height, 1
    require(path.suffix == ".dds" and len(header) == 128 and header[:4] == b"DDS "
            and struct.unpack_from("<I", header, 4)[0] == 124, f"Invalid DDS header: {path}")
    height, width = struct.unpack_from("<II", header, 12)
    mips = max(1, struct.unpack_from("<I", header, 28)[0])
    require(mips == max(width, height).bit_length(), f"Incomplete world-texture mip chain: {path}")
    return width, height, mips


def validate_textures(root):
    manifest = read_json(root, "manifest.json")
    require(manifest.get("schema_version") == 1, "Unsupported texture manifest schema")
    require(manifest.get("pack_id") == "nano-banana-2" and manifest.get("hash_version") == 5,
            "Unexpected bundled texture pack identity or hash version")
    database_path = check_file(root, "rt64.json", manifest["database_sha256"])
    database = json.loads(database_path.read_text(encoding="utf-8"))
    require(database.get("configuration", {}).get("hashVersion") == 5, "Expected RT64 v5 texture hashes")
    mappings = database.get("textures", [])
    require(bool(mappings) and len(mappings) == manifest["mapping_count"], "Texture mapping count mismatch")
    hashes, mapped_paths = set(), set()
    for mapping in mappings:
        key = mapping.get("hashes", {}).get("rt64", "")
        require(re.fullmatch(r"[0-9a-f]{16}", key) and key not in hashes, f"Invalid or duplicate RT64 hash: {key}")
        hashes.add(key)
        path = mapping["path"]
        require(PurePosixPath(path).parts[:1] == ("textures",), f"Texture must be inside textures/: {path}")
        member(root, path)
        mapped_paths.add(path)
    files = manifest["files"]
    require(len(files) == len(mapped_paths) == manifest["image_count"], "Texture image count mismatch")
    require({item["path"] for item in files} == mapped_paths, "Texture manifest and RT64 paths differ")
    total_bytes = 0
    for item in files:
        path = check_file(root, item["path"], item["sha256"], item["bytes"])
        require(image_header(path) == (item["width"], item["height"], item["mip_levels"]),
                f"Texture dimensions or mip levels disagree with manifest: {path}")
        require(item["width"] > 0 and item["height"] > 0, f"Empty texture dimensions: {path}")
        total_bytes += item["bytes"]
    require(total_bytes == manifest["total_image_bytes"], "Texture byte total mismatch")
    credits = member(root, manifest["credits_file"])
    check_contents(root, mapped_paths | {"manifest.json", "rt64.json", credits.relative_to(root).as_posix()},
                   optional=("README.md",))
    return {"mappings": len(mappings), "images": len(files), "image_bytes": total_bytes,
            "database_sha256": manifest["database_sha256"], "manifest_sha256": digest(root / "manifest.json")}


def fingerprint(assets):
    return {path.relative_to(assets).as_posix(): digest(path)
            for directory in (assets / "music", assets / PACK)
            for path in directory.rglob("*") if path.is_file()}


def validate(assets, reference=None):
    assets = Path(assets)
    report = {"music": validate_music(assets / "music"), "textures": validate_textures(assets / PACK)}
    if reference is not None:
        require(fingerprint(assets) == fingerprint(Path(reference)),
                "Bundled music/textures differ from the source checkout; rebuild/stage the app before packaging.")
    return report


def stage(source, destination):
    """Refresh only the two managed packs, retaining menus and unrelated assets."""
    source, destination = Path(source), Path(destination)
    report = validate(source)
    if source.resolve() == destination.resolve():
        return report
    for relative in (Path("music"), PACK):
        src, dst = source / relative, destination / relative
        require(not dst.is_symlink(), f"Refusing to stage into an asset symlink: {dst}")
        require(not dst.resolve().is_relative_to(src.resolve())
                and not src.resolve().is_relative_to(dst.resolve()), "Source and destination packs overlap")
        wanted = set()
        for path in src.rglob("*"):
            if not path.is_file():
                continue
            name = path.relative_to(src)
            wanted.add(name)
            target = dst / name
            require(target.resolve().is_relative_to(dst.resolve()), f"Staged asset escapes its pack: {target}")
            require(not target.is_symlink(), f"Refusing to overwrite asset symlink: {target}")
            if not target.is_file() or digest(target) != digest(path):
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, target)
        # Old pack revisions may have filenames no longer in the database.
        for path in dst.rglob("*"):
            if path.is_file() and path.relative_to(dst) not in wanted:
                path.unlink()
    return validate(destination, reference=source)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", type=Path, default=ROOT / "assets", help="Source assets directory")
    parser.add_argument("--stage", type=Path, help="Stage both packs into this app assets directory")
    parser.add_argument("--reference", type=Path, help="Require an exact match to these source assets")
    args = parser.parse_args()
    try:
        report = stage(args.assets, args.stage) if args.stage else validate(args.assets, args.reference)
    except (OSError, ValueError, KeyError, TypeError, struct.error, wave.Error) as error:
        parser.exit(1, f"Bundled asset validation failed: {error}\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
