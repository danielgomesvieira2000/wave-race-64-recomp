#!/usr/bin/env python3
"""Create a separate RT64 DDS pack with complete alpha-aware mip chains.

The input is an assembled PNG pack containing rt64.json. Original PNG pixels
become the exact top mip; lower mips use premultiplied area filtering and are
stored as straight-alpha R8G8B8A8_UNORM, matching RT64's PNG upload format.
The DX10 DDS header/layout follows lib/RT64/src/contrib/ddspp/ddspp.h.

Use a schema_version=1 policy with preserve_png_paths and/or preserve_png_hashes
arrays to keep UI/text mappings on the single-mip PNG path. Hash exclusions can
split mappings that previously shared a PNG. This tool does not install or
enable the new pack. Output must be a fresh ignored build*/ directory.
Requires numpy and Pillow. --validator can run the actual ddspp probe built from
tools/tests/ddspp_probe.cpp over every produced DDS.
"""
from __future__ import annotations
import argparse
import copy
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import struct
import subprocess

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
DXGI_RGBA8_UNORM = 28
DDS_HEADER_BYTES = 148


def area_resize_axis(values: np.ndarray, count: int, axis: int) -> np.ndarray:
    """Integrate source pixel boxes exactly, including odd and single-pixel axes."""
    source = np.moveaxis(values, axis, 0)
    length = source.shape[0]
    if length == count:
        return values.copy()
    integral = np.concatenate((np.zeros_like(source[:1]), np.cumsum(source, axis=0)), axis=0)
    edges = np.linspace(0.0, float(length), count + 1)
    indices = np.floor(edges).astype(np.int64)
    fractions = (edges - indices).reshape((-1,) + (1,) * (source.ndim - 1))
    samples = integral[indices] + source[np.minimum(indices, length - 1)] * fractions
    reduced = np.diff(samples, axis=0) * (count / length)
    return np.moveaxis(reduced, 0, axis)


def mip_chain(image: Image.Image) -> list[tuple[int, int, bytes]]:
    original = np.asarray(image.convert('RGBA'), dtype=np.uint8)
    height, width = original.shape[:2]
    levels = [(width, height, original.tobytes())]
    # Filter in RT64's existing encoded UNORM color space; switching the DDS to
    # an sRGB format would change material color relative to the PNG source.
    premultiplied = original.astype(np.float64)
    premultiplied[..., :3] *= premultiplied[..., 3:4] / 255.0
    while width > 1 or height > 1:
        width, height = max(1, width // 2), max(1, height // 2)
        premultiplied = area_resize_axis(premultiplied, width, 1)
        premultiplied = area_resize_axis(premultiplied, height, 0)
        straight = premultiplied.copy()
        alpha = straight[..., 3:4]
        straight[..., :3] = np.divide(premultiplied[..., :3] * 255.0, alpha,
            out=np.zeros_like(premultiplied[..., :3]), where=alpha > 1e-12)
        pixels = np.clip(np.floor(straight + 0.5), 0, 255).astype(np.uint8)
        levels.append((width, height, pixels.tobytes()))
    return levels


def dds_bytes(image: Image.Image) -> tuple[bytes, list[dict]]:
    levels = mip_chain(image)
    width, height, _ = levels[0]
    mip_count = len(levels)
    # DDSD_CAPS|HEIGHT|WIDTH|PITCH|PIXELFORMAT, plus MIPMAPCOUNT when applicable.
    flags = 0x100f | (0x20000 if mip_count > 1 else 0)
    caps = 0x1000 | (0x400008 if mip_count > 1 else 0)
    fields = [124, flags, height, width, width * 4, 1, mip_count] + [0] * 11
    fields += [32, 4, struct.unpack('<I', b'DX10')[0], 0, 0, 0, 0, 0]
    fields += [caps, 0, 0, 0, 0]
    # DXGI_Texture2D=3 even for 1xN; array size=1, no cubemap or special alpha.
    header = b'DDS ' + struct.pack('<31I', *fields) + struct.pack('<5I', DXGI_RGBA8_UNORM, 3, 0, 1, 0)
    assert len(header) == DDS_HEADER_BYTES
    metadata, offset = [], 0
    for index, (w, h, pixels) in enumerate(levels):
        metadata.append(dict(level=index, width=w, height=h, offset=offset,
                             row_pitch=w * 4, bytes=len(pixels)))
        offset += len(pixels)
    return header + b''.join(pixels for _, _, pixels in levels), metadata


def source_path(root: Path, relative: str) -> Path:
    if not isinstance(relative, str) or not relative or '\\' in relative:
        raise ValueError('texture path must be a nonempty forward-slash relative path')
    path = PurePosixPath(relative)
    if path.is_absolute() or '..' in path.parts:
        raise ValueError(f'texture path leaves source pack: {relative}')
    resolved = (root / path).resolve()
    if not resolved.is_relative_to(root) or not resolved.is_file():
        raise ValueError(f'missing texture or path outside source pack: {relative}')
    if resolved.suffix.lower() != '.png':
        raise ValueError(f'expected an assembled PNG source: {relative}')
    return resolved


def build_pack(source: Path, output: Path, policy: dict | None = None, validator: Path | None = None) -> dict:
    source, output = source.resolve(), output.resolve()
    relative = output.relative_to(ROOT) if output.is_relative_to(ROOT) else Path()
    if not relative.parts or not relative.parts[0].startswith('build'):
        raise ValueError('output must be a fresh ignored build*/ directory in this checkout')
    if output.is_relative_to(source) or source.is_relative_to(output):
        raise ValueError('input and output packs must be separate, non-overlapping directories')
    if output.exists():
        raise ValueError('output already exists; choose a fresh directory to preserve prior evidence')
    policy = policy or {'schema_version': 1}
    if policy.get('schema_version') != 1:
        raise ValueError('policy must use schema_version 1')
    preserve_paths = set(policy.get('preserve_png_paths', []))
    preserve_hashes = set(policy.get('preserve_png_hashes', []))
    database = json.loads((source / 'rt64.json').read_text())
    mapped_paths = {entry['path'] for entry in database['textures']}
    mapped_hashes = {entry.get('hashes', {}).get('rt64') for entry in database['textures']}
    if preserve_paths - mapped_paths or preserve_hashes - mapped_hashes:
        raise ValueError('policy contains paths or hashes absent from the input pack')
    inputs = {relative: source_path(source, relative) for relative in mapped_paths}
    destinations = [str(PurePosixPath(relative).with_suffix('.dds')) for relative in inputs]
    if len(destinations) != len(set(destinations)):
        raise ValueError('multiple PNG paths collide after changing their extension to DDS')
    database = copy.deepcopy(database)
    output.mkdir(parents=True, exist_ok=False)
    converted, retained = {}, set()
    for entry in database['textures']:
        relative = entry['path']
        if relative in preserve_paths or entry.get('hashes', {}).get('rt64') in preserve_hashes:
            destination = output / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            if relative not in retained:
                shutil.copyfile(inputs[relative], destination)
                retained.add(relative)
            continue
        destination_relative = str(PurePosixPath(relative).with_suffix('.dds'))
        if destination_relative not in converted:
            with Image.open(inputs[relative]) as image:
                payload, levels = dds_bytes(image)
                source_pixels = image.convert('RGBA').tobytes()
            if payload[DDS_HEADER_BYTES:DDS_HEADER_BYTES + len(source_pixels)] != source_pixels:
                raise RuntimeError(f'top mip differs from original PNG: {relative}')
            destination = output / destination_relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(payload)
            converted[destination_relative] = dict(source_png=relative, mips=levels,
                source_png_sha256=hashlib.sha256(inputs[relative].read_bytes()).hexdigest(),
                top_mip_sha256=hashlib.sha256(source_pixels).hexdigest(),
                dds_sha256=hashlib.sha256(payload).hexdigest(), bytes=len(payload))
        entry['path'] = destination_relative
    # Retain assembled pack credits and review metadata without duplicating the
    # input PNG set. Their provenance is also recorded in mip-report.json.
    for path in source.iterdir():
        if path.is_file() and path.suffix.lower() in ('.json', '.md', '.txt') and path.name not in ('rt64.json', 'mip-report.json'):
            shutil.copyfile(path, output / path.name)
    validation = []
    if validator:
        paths = sorted(converted)
        for start in range(0, len(paths), 64):
            batch = paths[start:start + 64]
            result = subprocess.run([str(validator.resolve()), *(str(output / path) for path in batch)],
                                    check=True, text=True, capture_output=True)
            parsed = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
            if len(parsed) != len(batch):
                raise RuntimeError('ddspp validator did not report every converted file')
            for relative, descriptor in zip(batch, parsed):
                expected = converted[relative]['mips']
                expected_layout = [{key: value for key, value in level.items() if key != 'level'} for level in expected]
                if descriptor['format'] != DXGI_RGBA8_UNORM or descriptor['header_bytes'] != DDS_HEADER_BYTES or descriptor['mips'] != expected_layout:
                    raise RuntimeError(f'ddspp decoded a different upload layout: {relative}')
                validation.append(dict(path=relative, **descriptor))
        if len(validation) != len(converted):
            raise RuntimeError('ddspp validator did not report every converted file')
    report = dict(schema_version=1, source_manifest_sha256=hashlib.sha256((source / 'rt64.json').read_bytes()).hexdigest(),
        format='DX10 R8G8B8A8_UNORM; 2D; straight alpha',
        filtering='premultiplied exact-area reduction in encoded UNORM color space',
        review_status='draft; inspect world materials and retain PNG mappings for UI/text as needed',
        converted_images=len(converted), retained_png_images=len(retained), replacement_mappings=len(database['textures']),
        total_dds_bytes=sum(item['bytes'] for item in converted.values()), policy=policy,
        textures=converted, actual_ddspp_validation=validation)
    (output / 'rt64.json').write_text(json.dumps(database, indent=2) + '\n')
    (output / 'mip-report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--policy', type=Path)
    parser.add_argument('--validator', type=Path)
    args = parser.parse_args()
    policy = json.loads(args.policy.read_text()) if args.policy else None
    report = build_pack(args.input, args.output, policy, args.validator)
    print(json.dumps({key: report[key] for key in ('converted_images', 'retained_png_images', 'replacement_mappings', 'total_dds_bytes')}, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
