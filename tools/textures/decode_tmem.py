#!/usr/bin/env python3
"""Decode RT64 v5 TMEM dumps into lossless RGBA PNGs and a hash inventory.

The sampler is a byte-exact translation of RT64's TextureDecoder.hlsli and
Formats.hlsli, not a decoder for .rice.rdram or ROM byte layouts. RT64 already
undoes RDRAM's host-word XOR while loading TMEM, so these dumps need no endian
swap. Odd rows still swap adjacent 32-bit words, exactly as the shader does.

Only Python's standard library is needed. Generated assets must stay under an
ignored build*/ directory in this checkout; this tool does not publish ROM art.

Sampler derived from RT64, copyright (c) 2024 RT64 Contributors, MIT licensed.
See ../../lib/RT64/LICENSE for the full upstream license.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import zlib

REPO = Path(__file__).resolve().parents[2]
TMEM_BYTES = 4096
FORMATS = {0: "RGBA", 1: "YUV", 2: "CI", 3: "IA", 4: "I", 5: "DEPTH"}
SOURCE_FILES = (
    "lib/RT64/src/shaders/TextureDecoder.hlsli",
    "lib/RT64/src/shaders/Formats.hlsli",
    "lib/RT64/src/hle/rt64_rdp_tmem.cpp",
    "lib/RT64/src/hle/rt64_rdp.cpp",
    "lib/RT64/src/render/rt64_texture_cache.cpp",
)


def rgba16(value: int) -> tuple[int, int, int, int]:
    r, g, b = (value >> 11) & 31, (value >> 6) & 31, (value >> 1) & 31
    return (r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255 if value & 1 else 0


def ia16(value: int) -> tuple[int, int, int, int]:
    return value >> 8, value >> 8, value >> 8, value & 255


def integer(obj: dict, name: str, lower: int, upper: int) -> int:
    value = obj.get(name)
    if isinstance(value, bool) or not isinstance(value, int) or not lower <= value <= upper:
        raise ValueError(f"{name} must be an integer in [{lower}, {upper}], got {value!r}")
    return value


def validate_metadata(info: dict) -> tuple[int, int, dict, str]:
    if not isinstance(info, dict) or not isinstance(info.get("tile"), dict):
        raise ValueError("tile JSON must contain an object named tile")
    width, height = integer(info, "width", 1, 4096), integer(info, "height", 1, 4096)
    tile = info["tile"]
    integer(tile, "fmt", 0, 7)
    integer(tile, "siz", 0, 3)
    integer(tile, "line", 0, 65535)
    integer(tile, "tmem", 0, 65535)
    integer(tile, "palette", 0, 255)
    tlut = info.get("tlut")
    if tlut not in ("None", "RGBA16", "IA16"):
        raise ValueError(f"unknown RT64 LoadTLUT value {tlut!r}; expected None, RGBA16 or IA16")
    if tile["line"] == 0 and height > 1:
        raise ValueError("multi-row tile with line=0 has undefined odd-row division in the RT64 shader")
    return width, height, tile, tlut


def sample_tmem(tmem: bytes, x: int, y: int, tile: dict, tlut: str) -> tuple[int, int, int, int]:
    """Equivalent to sampleTMEM(), with float UNORM values expressed as bytes."""
    fmt, siz = tile["fmt"], tile["siz"]
    address, stride = tile["tmem"] << 3, tile["line"] << 3
    rgba32 = fmt == 0 and siz == 3
    mask = 0x7FF if rgba32 or tlut != "None" else 0xFFF
    shift = 2 if rgba32 else siz
    pixel = y * stride + ((x << shift) >> 1)

    def load(relative: int, bank: int = 0) -> int:
        # implLoadTMEM swaps words relative to the row, not to textureStart.
        if y & 1:
            row_start = (relative // stride) * stride
            word = (relative - row_start) // 4
            final = address + row_start + ((word ^ 1) * 4) + (relative & 3)
        else:
            final = address + relative
        return tmem[((final & mask) | bank) & 0xFFF]

    v0, v1 = load(pixel), load(pixel + 1)
    nibble = (v0 >> (0 if x & 1 else 4)) & 15
    if tlut != "None":
        # TLUT is enabled independently of fmt. Each entry occupies eight bytes.
        pa = 0x800 + ((tile["palette"] << 7) + (nibble << 3) if siz == 0 else v0 << 3)
        value = (tmem[pa & 0xFFF] << 8) | tmem[(pa + 1) & 0xFFF]
        return rgba16(value) if tlut == "RGBA16" else ia16(value)

    if fmt not in (0, 2, 3, 4):
        # TextureDecoder.hlsli deliberately returns opaque black for YUV/unknown.
        return 0, 0, 0, 255
    if siz == 0:
        if fmt == 3:
            i = nibble & 14
            i = (i << 4) | (i << 1) | (i >> 2)
            return i, i, i, 255 if nibble & 1 else 0
        i = ((tile["palette"] << 4) | nibble) if fmt == 2 else nibble * 17
        # Invalid palette fields can exceed UNORM; the GPU storage clamps them.
        i = min(i, 255)
        return i, i, i, i
    if siz == 1:
        if fmt == 3:
            i, a = (v0 >> 4) * 17, (v0 & 15) * 17
            return i, i, i, a
        return v0, v0, v0, v0
    if siz == 2:
        if fmt == 0:
            return rgba16((v0 << 8) | v1)
        if fmt == 3:
            return v0, v0, v0, v1
        return v0, v1, v0, v1
    p2, bank = (pixel, 0x800) if rgba32 else (pixel + 2, 0)
    v2, v3 = load(p2, bank), load(p2 + 1, bank)
    if rgba32:
        return v0, v1, v2, v3
    # Follow the shader's actual select(oddColumn, RG, BA) argument order.
    r, g = (v0, v1) if x & 1 else (v2, v3)
    return r, g, r, g


def decode_tmem(tmem: bytes, info: dict) -> bytes:
    if len(tmem) != TMEM_BYTES:
        raise ValueError(f"TMEM dump must be exactly {TMEM_BYTES} bytes, got {len(tmem)}")
    width, height, tile, tlut = validate_metadata(info)
    rgba = bytearray(width * height * 4)
    for y in range(height):
        for x in range(width):
            offset = (y * width + x) * 4
            rgba[offset:offset + 4] = bytes(sample_tmem(tmem, x, y, tile, tlut))
    return bytes(rgba)


def write_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    """Write RGBA8 with no color conversion, filtering, palette or alpha loss."""
    if len(rgba) != width * height * 4:
        raise ValueError("RGBA byte count does not match PNG dimensions")

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    rows = b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def alpha_summary(rgba: bytes) -> dict:
    alpha = rgba[3::4]
    transparent, opaque = alpha.count(0), alpha.count(255)
    return {"minimum": min(alpha), "maximum": max(alpha), "transparent_pixels": transparent,
            "opaque_pixels": opaque, "partial_pixels": len(alpha) - transparent - opaque}


def inventory(input_dir: Path, output_dir: Path) -> dict:
    if not input_dir.is_dir():
        raise ValueError(f"input is not a directory: {input_dir}")
    resolved = output_dir.resolve()
    if (not resolved.is_relative_to(REPO) or not resolved.relative_to(REPO).parts
            or not resolved.relative_to(REPO).parts[0].startswith("build")):
        raise ValueError("output must be under an ignored build*/ directory in this checkout")
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "images").mkdir(exist_ok=True)
    entries, groups, errors = [], {}, []
    sources = sorted(input_dir.rglob("*.v5.tile.json"))
    if not sources:
        raise ValueError("no *.v5.tile.json files found")
    for path in sources:
        name = path.name.removesuffix(".tile.json")
        tmem_path = path.with_name(name + ".tmem")
        entry = {"source_id": name, "source_tile_json": path.relative_to(input_dir).as_posix(),
                 "source_tmem": tmem_path.relative_to(input_dir).as_posix()}
        try:
            match = re.fullmatch(r"([0-9a-fA-F]{16})\.v5", name)
            if not match:
                raise ValueError("dump basename must be a 16-digit hexadecimal hash followed by .v5")
            entry.update({"hash": match[1].lower(), "hash_version": 5})
            info = json.loads(path.read_text())
            entry["metadata"] = info  # Preserve every original tile/root field.
            width, height, tile, tlut = validate_metadata(info)
            tmem = tmem_path.read_bytes()
            rgba = decode_tmem(tmem, info)
            pixels_sha = hashlib.sha256(rgba).hexdigest()
            image_id = hashlib.sha256(struct.pack(">II", width, height) + rgba).hexdigest()
            png = f"images/{image_id}.png"
            entry.update({"width": width, "height": height, "fmt": tile["fmt"], "siz": tile["siz"],
                          "format": f"{FORMATS.get(tile['fmt'], 'UNKNOWN')}{4 << tile['siz']}",
                          "tlut": tlut, "palette": tile["palette"], "tmem_start_bytes": tile["tmem"] << 3,
                          "stride_bytes": tile["line"] << 3, "tmem_sha256": hashlib.sha256(tmem).hexdigest(),
                          "rgba_sha256": pixels_sha, "image_id": image_id, "png": png, "alpha": alpha_summary(rgba)})
            if tile["fmt"] not in (0, 2, 3, 4) and tlut == "None":
                entry["warning"] = "RT64 TextureDecoder returns opaque black for this format; no YUV conversion is inferred."
            if image_id not in groups:
                write_png(output_dir / png, width, height, rgba)
                groups[image_id] = {"image_id": image_id, "png": png, "width": width, "height": height,
                                    "rgba_sha256": pixels_sha, "png_sha256": hashlib.sha256((output_dir / png).read_bytes()).hexdigest(),
                                    "hash_aliases": [], "source_ids": [], "alpha": entry["alpha"]}
            group = groups[image_id]
            for key, value in (("hash_aliases", entry["hash"]), ("source_ids", entry["source_id"])):
                if value not in group[key]:
                    group[key].append(value)
            entries.append(entry)
        except (OSError, ValueError, KeyError, TypeError, ZeroDivisionError) as error:
            entry["error"] = str(error)
            errors.append(entry)
    expected = {str(p.with_name(p.name.removesuffix(".tile.json") + ".tmem")) for p in sources}
    orphaned = [p.relative_to(input_dir).as_posix() for p in sorted(input_dir.rglob("*.v5.tmem")) if str(p) not in expected]
    result = {"schema_version": 1, "decoder": "RT64 TextureDecoder.hlsli / Formats.hlsli byte translation",
              "input_directory": str(input_dir.resolve()), "pixel_identity": "SHA-256 of big-endian uint32 width, height, then row-major RGBA8 bytes",
              "notes": ["TMEM bytes are decoded as dumped; no RDRAM/ROM endian swap is applied.",
                        "PNG dimensions and alpha are preserved. Identical RGBA pixels at identical dimensions share one PNG.",
                        "Hashes are preserved from RT64 filenames; this tool does not recompute replacement hashes.",
                        "Tile clamp, mirror, UV bounds and shifts are retained in metadata, not baked into the source image."],
              "ground_truth": [{"path": s, "sha256": hashlib.sha256((REPO / s).read_bytes()).hexdigest()} for s in SOURCE_FILES],
              "summary": {"source_pairs": len(sources), "decoded_sources": len(entries), "unique_images": len(groups),
                          "pixel_duplicate_sources": len(entries) - len(groups), "errors": len(errors), "orphaned_tmem_files": len(orphaned)},
              "textures": entries, "images": list(groups.values()), "errors": errors, "orphaned_tmem_files": orphaned}
    (output_dir / "inventory.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path, help="directory containing RT64 .v5.tmem/.v5.tile.json pairs (searched recursively)")
    parser.add_argument("--output", required=True, type=Path, help="ignored build*/ output directory")
    args = parser.parse_args()
    try:
        result = inventory(args.input, args.output)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps(result["summary"]))
    return 1 if result["errors"] or result["orphaned_tmem_files"] else 0


if __name__ == "__main__":
    sys.exit(main())
