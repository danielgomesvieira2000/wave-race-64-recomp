#!/usr/bin/env python3
"""Prepare source-faithful 1024x768 texture plates and assemble reviewed 4x art.

No generation service is called. `prepare` consumes decode_tmem inventory.json;
`assemble` consumes generated plates with the same plate-NNNN.png filenames.
Assembly produces draft replacements and QA measurements, never visual approval.

Synthetic joined inventory images may supply regions=[{id, rect:[l,t,r,b],
hash_aliases:[...]}]. Rectangles use original source pixels and are split only
after the joined image has been processed. Unspecified regions cover the image.
Requires Pillow and numpy. All outputs stay in ignored build*/ directories.
"""
from __future__ import annotations
import argparse
import colorsys
import hashlib
import json
from pathlib import Path
import re
import sys

import numpy as np
from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parents[2]
PLATE_SIZE = (1024, 768)
BACKGROUND = (64, 64, 64)
RESAMPLE = Image.Resampling.LANCZOS


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def output_directory(path: Path) -> Path:
    path = path.resolve()
    relative = path.relative_to(ROOT) if path.is_relative_to(ROOT) else Path()
    if not relative.parts or not relative.parts[0].startswith('build'):
        raise ValueError('output must be under an ignored build*/ directory in this checkout')
    path.mkdir(parents=True, exist_ok=True)
    return path


def resize_rgba(image: Image.Image, size: tuple[int, int]) -> Image.Image:
    # Explicit premultiplied interpolation prevents transparent colors bleeding.
    result = image.convert('RGBa').resize(size, RESAMPLE).convert('RGBA')
    result.putalpha(image.getchannel('A').resize(size, RESAMPLE))
    return result


def float_resize(array: np.ndarray, size: tuple[int, int], resample=RESAMPLE) -> np.ndarray:
    if array.ndim == 2:
        return np.asarray(Image.fromarray(array.astype(np.float32)).resize(size, resample), dtype=np.float32)
    return np.stack([float_resize(array[..., i], size, resample) for i in range(array.shape[2])], axis=2)


def axis_indices(size: int, border: int, mode: str) -> np.ndarray:
    index = np.arange(-border, size + border)
    if mode == 'wrap':
        return index % size
    if mode == 'mirror':
        phase = index % (size * 2)
        return np.where(phase < size, phase, size * 2 - 1 - phase)
    return np.clip(index, 0, size - 1)


def pad_rgba(image: Image.Image, border: int, s: str, t: str) -> Image.Image:
    pixels = np.asarray(image)
    return Image.fromarray(pixels[axis_indices(image.height, border, t)[:, None],
                                 axis_indices(image.width, border, s)[None, :]])


def classify(image: Image.Image) -> tuple[str, int]:
    pixels = np.asarray(image, dtype=np.float32) / 255
    alpha = pixels[..., 3]
    kind = 'opaque' if np.all(alpha == 1) else ('cutout' if np.all((alpha == 0) | (alpha == 1)) else 'soft-alpha')
    rgb = (pixels[..., :3] * alpha[..., None]).sum(axis=(0, 1)) / max(float(alpha.sum()), 1e-8)
    h, saturation, _ = colorsys.rgb_to_hsv(*rgb)
    # A color grouping aid, not semantic material classification.
    color = 6 if saturation < .15 else min(5, int(h * 6))
    return kind, color


def image_regions(record: dict, size: tuple[int, int]) -> list[dict]:
    regions = record.get('regions') or [{'id': record['image_id'], 'rect': [0, 0, *size],
                                       'hash_aliases': record.get('hash_aliases', [])}]
    checked = []
    for index, region in enumerate(regions):
        rect = region.get('rect')
        if (not isinstance(rect, list) or len(rect) != 4 or any(type(v) is not int for v in rect)
                or not 0 <= rect[0] < rect[2] <= size[0] or not 0 <= rect[1] < rect[3] <= size[1]):
            raise ValueError(f'invalid source region {rect!r} in {record["image_id"]}')
        aliases = region.get('hash_aliases', [])
        if not aliases or any(not isinstance(h, str) or not re.fullmatch(r'[0-9a-fA-F]{16}', h) for h in aliases):
            raise ValueError(f'region {index} in {record["image_id"]} needs real 16-hex RT64 hash aliases')
        checked.append({**region, 'id': region.get('id', f'{record["image_id"]}-{index}'),
                        'hash_aliases': sorted(set(h.lower() for h in aliases)),
                        'original_size': [rect[2] - rect[0], rect[3] - rect[1]]})
    return checked


def sampling_modes(regions: list[dict], textures: list[dict], size: tuple[int, int]) -> dict:
    aliases = {h for region in regions for h in region['hash_aliases']}
    matching = [t for t in textures if t.get('hash', '').lower() in aliases]
    result = {'s': 'edge', 't': 'edge', 'observed': {}, 'warnings': []}
    for axis, field, mask, extent in [('s', 'cms', 'masks', size[0]), ('t', 'cmt', 'maskt', size[1])]:
        modes = set()
        for texture in matching:
            tile = texture.get('metadata', {}).get('tile', {})
            if field not in tile or mask not in tile:
                result['warnings'].append(f'{axis}: missing sampler fields; using edge padding')
                modes.add('edge'); continue
            flags, bits = int(tile[field]), int(tile[mask])
            # rt64_state.cpp sets G_TX_CLAMP when masks/maskt is zero.
            if flags & 2 or bits == 0:
                modes.add('edge')
            elif (1 << bits) != extent:
                modes.add('edge')
                result['warnings'].append(f'{axis}: sampled period {1 << bits} differs from joined/source extent {extent}; edge padding avoids inventing missing texels')
            else:
                modes.add('mirror' if flags & 1 else 'wrap')
        if not modes:
            result['warnings'].append(f'{axis}: no sampler metadata; using edge padding')
            modes.add('edge')
        result['observed'][axis] = sorted(modes)
        # A shared source can have different draw modes. A real repeat has priority
        # in the conditioning border; every observed mode is retained for review.
        result[axis] = next(m for m in ('wrap', 'mirror', 'edge') if m in modes)
        if len(modes) > 1:
            result['warnings'].append(f'{axis}: aliases have mixed sampler modes {sorted(modes)}')
    result['warnings'] = sorted(set(result['warnings']))
    return result


def prepare(inventory_path: Path, output: Path, grid=(8, 6), tile_size=None, border=8, gutter=8) -> dict:
    output = output_directory(output)
    inventory = json.loads(inventory_path.read_text())
    if inventory.get('errors'):
        raise ValueError('inventory contains decoding errors; use a deliberately selected error-free inventory')
    columns, rows = grid
    if grid not in ((1, 1), (2, 1), (4, 3), (8, 6)) or border < 0 or gutter < 0:
        raise ValueError('grid must be 1x1, 2x1, 4x3 or 8x6; border/gutter must be nonnegative')
    cell_width, cell_height = PLATE_SIZE[0] // columns, PLATE_SIZE[1] // rows
    capacity = min(cell_width, cell_height) - 2 * (border + gutter)
    tile_size = capacity if tile_size is None else tile_size
    if not 1 <= tile_size <= capacity:
        raise ValueError(f'tile-size must be between 1 and {capacity} for these cell margins')
    for directory in ('plates', 'sources', 'alpha'):
        (output / directory).mkdir(exist_ok=True)
    records = []
    all_aliases, all_image_ids = set(), set()
    alias_dimensions = {}
    for texture in inventory.get('textures', []):
        metadata = texture.get('metadata', {})
        width = texture.get('width', metadata.get('width'))
        height = texture.get('height', metadata.get('height'))
        if width is not None and height is not None:
            alias_dimensions.setdefault(texture.get('hash', '').lower(), set()).add((width, height))
    for record in inventory.get('images', []):
        source_path = inventory_path.parent / record['png']
        with Image.open(source_path) as opened:
            image = opened.convert('RGBA')
        if list(image.size) != [record['width'], record['height']]:
            raise ValueError(f'source dimensions differ from inventory: {source_path}')
        rgba_hash = hashlib.sha256(image.tobytes()).hexdigest()
        if record.get('rgba_sha256') and rgba_hash != record['rgba_sha256']:
            raise ValueError(f'source pixels differ from inventory: {source_path}')
        image_id = record['image_id']
        if not re.fullmatch(r'[A-Za-z0-9_-]+', image_id):
            raise ValueError(f'unsafe image_id {image_id!r}')
        if image_id in all_image_ids:
            raise ValueError(f'duplicate image_id {image_id!r} would overwrite its trusted source')
        all_image_ids.add(image_id)
        regions = image_regions(record, image.size)
        for region in regions:
            if all_aliases.intersection(region['hash_aliases']):
                raise ValueError('the same RT64 hash cannot map to different source regions')
            dimensions = tuple(region['original_size'])
            for alias in region['hash_aliases']:
                observed = alias_dimensions.get(alias, set())
                if observed and observed != {dimensions}:
                    raise ValueError(f'source region {region["id"]!r} dimensions {dimensions} '
                                     f'differ from RT64 hash {alias} dimensions {sorted(observed)}')
            all_aliases.update(region['hash_aliases'])
        image.save(output / 'sources' / f'{image_id}.png')
        image.getchannel('A').save(output / 'alpha' / f'{image_id}.png')
        records.append({'record': record, 'image': image, 'regions': regions, 'rgba_sha256': rgba_hash,
                        'group': classify(image), 'sampling': sampling_modes(regions, inventory.get('textures', []), image.size)})
    if not records:
        raise ValueError('inventory has no images')
    records.sort(key=lambda r: (*r['group'], r['record']['image_id']))
    plates, cells = [], []
    # Adjacent alpha/color groups share batches when needed to avoid paying for
    # mostly empty plates. No labels or decorative marks touch the plates.
    for group in [records]:
        alpha_group = 'sorted-alpha-color'
        for batch_start in range(0, len(group), columns * rows):
            plate_id = f'plate-{len(plates):04d}'
            plate = Image.new('RGB', PLATE_SIZE, BACKGROUND)
            plate_cells = []
            for slot, item in enumerate(group[batch_start:batch_start + columns * rows]):
                original = item['image']; image_id = item['record']['image_id']
                scale = min(tile_size / original.width, tile_size / original.height)
                fitted = (max(1, round(original.width * scale)), max(1, round(original.height * scale)))
                col, row = slot % columns, slot // columns
                left = col * cell_width + (cell_width - fitted[0]) // 2
                top = row * cell_height + (cell_height - fitted[1]) // 2
                rect = [left, top, left + fitted[0], top + fitted[1]]
                preview = pad_rgba(resize_rgba(original, fitted), border, item['sampling']['s'], item['sampling']['t'])
                backing = Image.new('RGBA', preview.size, (*BACKGROUND, 255))
                backing.alpha_composite(preview)
                plate.paste(backing.convert('RGB'), (left - border, top - border))
                cell = {'image_id': image_id, 'plate_id': plate_id, 'slot': slot,
                        'original_size': list(original.size), 'source_rgba': f'sources/{image_id}.png',
                        'source_alpha': f'alpha/{image_id}.png', 'source_rgba_sha256': item['rgba_sha256'],
                        'source_alpha_sha256': hashlib.sha256(original.getchannel('A').tobytes()).hexdigest(),
                        'hash_aliases': sorted(h for region in item['regions'] for h in region['hash_aliases']),
                        'regions': item['regions'], 'sampling': item['sampling'],
                        'group': {'alpha': item['group'][0], 'color_bin': item['group'][1]},
                        'cell_rect': [col * cell_width, row * cell_height, (col + 1) * cell_width, (row + 1) * cell_height],
                        'crop_rect': rect, 'padding_rect': [left - border, top - border, rect[2] + border, rect[3] + border],
                        'crop_normalized': [rect[0] / 1024, rect[1] / 768, rect[2] / 1024, rect[3] / 768],
                        'source_record_metadata': {k: v for k, v in item['record'].items() if k not in ('png', 'regions')}}
                cells.append(cell); plate_cells.append(image_id)
            filename = f'plates/{plate_id}.png'; plate.save(output / filename)
            plates.append({'id': plate_id, 'file': filename, 'size': list(PLATE_SIZE),
                           'sha256': sha(output / filename), 'alpha_group': alpha_group, 'image_ids': plate_cells})
    manifest = {'schema_version': 1, 'inventory_sha256': sha(inventory_path), 'plate_size': list(PLATE_SIZE),
                'background_rgb': list(BACKGROUND), 'grid': list(grid), 'tile_size': tile_size, 'border': border, 'gutter': gutter,
                'target_scale': 4, 'crop_convention': '[left, top, right, bottom], right/bottom exclusive',
                'notes': ['Color bins are grouping aids, not semantic material labels.',
                          'Generated plate must retain layout and framing. Normalized crop coordinates tolerate dimension changes, not relocated or redrawn cells.',
                          'Assembly restores trusted alpha, corrects source low-frequency RGB and retains source edge strips; it does not approve generative changes.'],
                'plates': plates, 'cells': cells}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def weighted_low(rgb: np.ndarray, alpha: np.ndarray, size: tuple[int, int]) -> np.ndarray:
    weight = float_resize(alpha, size, Image.Resampling.BOX)
    return float_resize(rgb * alpha[..., None], size, Image.Resampling.BOX) / np.maximum(weight[..., None], 1e-5)


def color_metrics(rgb: np.ndarray, alpha: np.ndarray, source: np.ndarray) -> dict:
    low = weighted_low(rgb, alpha, (source.shape[1], source.shape[0]))
    visible = source[..., 3] > .1
    if not np.any(visible):
        return {'low_frequency_rgb_rmse': 0., 'luminance_correlation': None, 'luminance_contrast_ratio': None}
    difference = low[visible] - source[..., :3][visible]
    luma = np.array([.2126, .7152, .0722])
    a, b = low[visible] @ luma, source[..., :3][visible] @ luma
    correlation = None if np.std(a) < .015 or np.std(b) < .015 else float(np.corrcoef(a, b)[0, 1])
    return {'low_frequency_rgb_rmse': float(np.sqrt(np.mean(difference ** 2))), 'luminance_correlation': correlation,
            'luminance_contrast_ratio': None if np.std(b) < .015 else float(np.std(a) / np.std(b))}


def detail_boundary_mask(source: np.ndarray, size: tuple[int, int]) -> np.ndarray:
    """Protect native color/alpha boundaries, with an outward-only soft border."""
    alpha = source[..., 3]
    boundary = np.zeros(alpha.shape, dtype=bool)
    for axis in (0, 1):
        a = source[:-1] if axis == 0 else source[:, :-1]
        b = source[1:] if axis == 0 else source[:, 1:]
        color_edge = (np.max(np.abs(a[..., :3] - b[..., :3]), axis=2) > .12)
        color_edge &= (np.minimum(a[..., 3], b[..., 3]) > .05)
        edge = color_edge | (np.abs(a[..., 3] - b[..., 3]) > .1)
        if axis == 0:
            boundary[:-1] |= edge; boundary[1:] |= edge
        else:
            boundary[:, :-1] |= edge; boundary[:, 1:] |= edge
    native = Image.fromarray(boundary.astype(np.uint8) * 255).filter(ImageFilter.MaxFilter(3))
    protected = native.resize(size, Image.Resampling.NEAREST)
    # The blur may extend protection outside the marked texels, but must never
    # weaken protection inside a glyph stroke or an opaque painted boundary.
    return np.maximum(np.asarray(protected, dtype=np.float32),
                      np.asarray(protected.filter(ImageFilter.GaussianBlur(1)), dtype=np.float32)) / 255


def reconstruct(crop: Image.Image, original: Image.Image, detail_strength=.75, edge_width=4,
                policy: dict | None = None) -> tuple[Image.Image, dict]:
    size = (original.width * 4, original.height * 4)
    trusted_image = resize_rgba(original, size)
    reference = np.asarray(trusted_image, dtype=np.float32) / 255
    source = np.asarray(original, dtype=np.float32) / 255
    alpha, trusted = reference[..., 3], reference[..., :3]
    composite = np.asarray(crop.convert('RGB'), dtype=np.float32) / 255
    candidate = np.clip((composite - (1 - alpha[..., None]) * (64 / 255)) / np.maximum(alpha[..., None], .05), 0, 1)
    # Nearly invisible texels cannot be reliably uncomposited from a gray plate.
    confidence = np.clip((alpha - .05) / .2, 0, 1)[..., None]
    candidate = trusted + confidence * (candidate - trusted)
    raw_metrics = color_metrics(candidate, alpha, source)
    requested_detail = detail_strength
    fallback_reasons = []
    visible = source[..., 3] > .05
    source_colors = source[..., :3][visible]
    if not np.any(visible) or np.all(np.ptp(source_colors, axis=0) <= 1 / 255 + 1e-6):
        fallback_reasons.append('flat_source_color')
    if (source_colors.size and np.all(np.ptp(source_colors, axis=1) <= 2 / 255 + 1e-6)
            and np.any((source[..., 3] > 0) & (source[..., 3] < 1))):
        fallback_reasons.append('grayscale_soft_effect_or_ui')
    raw_correlation = raw_metrics['luminance_correlation']
    if (raw_metrics['low_frequency_rgb_rmse'] > .30
            or (raw_correlation is not None and raw_correlation < .35
                and raw_metrics['low_frequency_rgb_rmse'] > .12)):
        fallback_reasons.append('generated_candidate_failed_structure_guard')
    if policy is not None:
        value = policy.get('detail_strength', detail_strength)
        if not isinstance(value, (int, float)) or not 0 <= value <= 1:
            raise ValueError('per-image policy detail_strength must be 0..1')
        detail_strength = min(detail_strength, value)
        if detail_strength == 0:
            fallback_reasons.append('visual_review_override: ' + str(policy.get('reason', 'preserve source')))
    if fallback_reasons:
        detail_strength = 0
    corrected = trusted + detail_strength * (candidate - trusted)
    for _ in range(3):
        low = weighted_low(corrected, alpha, original.size)
        correction = (source[..., :3] - low) * (source[..., 3:4] > .1)
        corrected = np.clip(corrected + float_resize(correction, size), 0, 1)
    protected = detail_boundary_mask(source, size)
    corrected = trusted + (1 - protected[..., None]) * (corrected - trusted)
    if edge_width:
        y, x = np.indices((size[1], size[0]))
        distance = np.minimum.reduce([x, size[0] - 1 - x, y, size[1] - 1 - y])
        edge_weight = np.clip(distance / edge_width, 0, 1)[..., None]
        corrected = trusted + edge_weight * (corrected - trusted)
    corrected = np.where((alpha > 0)[..., None], corrected, trusted)
    rgba = np.dstack([np.rint(corrected * 255).clip(0, 255).astype(np.uint8), np.rint(alpha * 255).astype(np.uint8)])
    if detail_strength == 0:
        # Rejected candidates bypass even source color correction: these outputs
        # are exactly the trusted resample and are not claimed as generated art.
        rgba = np.asarray(trusted_image).copy()
    final_metrics = color_metrics(rgba[..., :3].astype(np.float32) / 255, alpha, source)
    flags = []
    if raw_metrics['low_frequency_rgb_rmse'] > .18:
        flags.append('large_generated_color_or_layout_change')
    correlation = raw_metrics['luminance_correlation']
    if correlation is not None and correlation < .65:
        flags.append('generated_luminance_structure_changed')
    contrast = raw_metrics['luminance_contrast_ratio']
    if contrast is not None and contrast < .35:
        flags.append('generated_luminance_structure_lost')
    if final_metrics['low_frequency_rgb_rmse'] > .06:
        flags.append('remaining_low_frequency_difference')
    uses_generated_detail = not np.array_equal(rgba, np.asarray(trusted_image))
    metrics = {'raw_generated': raw_metrics, 'final': final_metrics,
               'candidate_status': 'protected_nb2_detail' if uses_generated_detail else 'source_preserving_4x',
               'requested_detail_strength': requested_detail, 'applied_detail_strength': detail_strength,
               'fallback_reasons': fallback_reasons,
               'visual_review_policy': policy,
               'native_boundary_protected_fraction': float(np.mean(protected >= 1)),
               'native_boundary_rgb_max_error_u8': int(np.max(np.abs(rgba[..., :3].astype(int) - np.asarray(trusted_image)[..., :3].astype(int))[protected >= 1], initial=0)),
               'alpha_matches_trusted_resample': np.array_equal(rgba[..., 3], np.asarray(original.getchannel('A').resize(size, RESAMPLE))),
               'source_edge_rgb_max_error_u8': int(max(np.max(np.abs(rgba[0, :, :3].astype(int) - np.rint(trusted[0] * 255).astype(int))),
                                                       np.max(np.abs(rgba[-1, :, :3].astype(int) - np.rint(trusted[-1] * 255).astype(int))),
                                                       np.max(np.abs(rgba[:, 0, :3].astype(int) - np.rint(trusted[:, 0] * 255).astype(int))),
                                                       np.max(np.abs(rgba[:, -1, :3].astype(int) - np.rint(trusted[:, -1] * 255).astype(int))))),
               'flags': flags, 'review_status': 'visual_review_required'}
    return Image.fromarray(rgba), metrics


def assemble(manifest_path: Path, generated: Path, output: Path, detail_strength=.75, edge_width=4,
             policy_path: Path | None = None) -> dict:
    output = output_directory(output)
    if not 0 <= detail_strength <= 1 or edge_width < 1:
        raise ValueError('detail-strength must be 0..1 and edge-width at least 1')
    manifest = json.loads(manifest_path.read_text())
    if manifest.get('schema_version') != 1 or manifest.get('target_scale') != 4:
        raise ValueError('unsupported atlas manifest')
    policies = {}
    if policy_path is not None:
        policy_document = json.loads(policy_path.read_text())
        if policy_document.get('schema_version') != 1 or not isinstance(policy_document.get('images'), dict):
            raise ValueError('unsupported visual review policy')
        policies = policy_document['images']
        if any(not isinstance(value, dict) for value in policies.values()):
            raise ValueError('every per-image policy must be an object')
    base = manifest_path.parent
    (output / 'textures').mkdir(exist_ok=True)
    results, mappings, plate_records = [], {}, {}
    for plate in manifest['plates']:
        path = generated / f'{plate["id"]}.png'
        if not path.is_file():
            raise ValueError(f'missing generated plate {path}')
        with Image.open(path) as opened:
            image = opened.convert('RGB')
        plate_records[plate['id']] = {'image': image, 'size': list(image.size), 'file': str(path.resolve()), 'sha256': sha(path),
                                     'aspect_ratio_error': abs((image.width / image.height) / (1024 / 768) - 1)}
    for cell in manifest['cells']:
        with Image.open(base / cell['source_rgba']) as opened:
            original = opened.convert('RGBA')
        if hashlib.sha256(original.tobytes()).hexdigest() != cell['source_rgba_sha256']:
            raise ValueError(f'trusted source changed for {cell["image_id"]}')
        plate = plate_records[cell['plate_id']]; image = plate['image']
        n = cell['crop_normalized']
        rect = (n[0] * image.width, n[1] * image.height, n[2] * image.width, n[3] * image.height)
        target_size = (original.width * 4, original.height * 4)
        # EXTENT samples float crop coordinates directly; rounding the crop first
        # would introduce cell-dependent shifts when the output dimensions differ.
        crop = image.transform(target_size, Image.Transform.EXTENT, rect, Image.Resampling.BICUBIC)
        replacement, qa = reconstruct(crop, original, detail_strength, edge_width, policies.get(cell['image_id']))
        period = cell.get('source_record_metadata', {}).get('periodic_y')
        if period is not None:
            if type(period) is not int or period <= 0 or original.height % period:
                raise ValueError('periodic_y must exactly divide the source height')
            source_pixels = np.asarray(original)
            expected = np.tile(source_pixels[:period], (original.height // period, 1, 1))
            if not np.array_equal(source_pixels, expected):
                raise ValueError('declared scroll parent is not exactly periodic in source pixels')
            # Generate the material once. Every scroll phase samples the same
            # details, including when its window crosses the periodic boundary.
            pixels = np.asarray(replacement).copy()
            core = pixels[:period * 4].copy()
            pixels = np.tile(core, (original.height // period, 1, 1))
            replacement = Image.fromarray(pixels)
            qa['periodic_y_source_pixels'] = period
            qa['periodic_repeats_exact'] = True
            qa['final'] = color_metrics(pixels[..., :3].astype(np.float32) / 255,
                                        pixels[..., 3].astype(np.float32) / 255,
                                        source_pixels.astype(np.float32) / 255)
        qa['sampled_crop_rect'] = list(rect)
        qa['generated_crop_pixels_per_target_pixel'] = [(rect[2] - rect[0]) / target_size[0], (rect[3] - rect[1]) / target_size[1]]
        if plate['aspect_ratio_error'] > .02:
            qa['flags'].append('generated_plate_aspect_changed')
        if min(qa['generated_crop_pixels_per_target_pixel']) < .75:
            qa['flags'].append('generated_crop_has_insufficient_resolution')
        region_outputs = []
        for region in cell['regions']:
            rect4 = tuple(v * 4 for v in region['rect'])
            fragment = replacement.crop(rect4)
            region_id = hashlib.sha256((cell['image_id'] + json.dumps(region['rect'])).encode()).hexdigest()
            path = f'textures/{region_id}.png'; fragment.save(output / path)
            for alias in region['hash_aliases']:
                if alias in mappings and mappings[alias] != path:
                    raise ValueError(f'conflicting RT64 mapping for {alias}')
                mappings[alias] = path
            region_outputs.append({'id': region['id'], 'path': path, 'source_rect': region['rect'],
                                   'original_size': region['original_size'], 'output_size': list(fragment.size),
                                   'hash_aliases': region['hash_aliases'], 'png_sha256': sha(output / path)})
        results.append({'image_id': cell['image_id'], 'plate_id': cell['plate_id'], 'qa': qa, 'regions': region_outputs,
                        'sampler_warnings': cell['sampling']['warnings']})
    database = {'configuration': {'autoPath': 'rt64', 'configurationVersion': 3, 'hashVersion': 5,
                                  'defaultOperation': 'stream', 'defaultShift': 'half'},
                'textures': [{'hashes': {'rt64': alias}, 'path': path} for alias, path in sorted(mappings.items())]}
    (output / 'rt64.json').write_text(json.dumps(database, indent=2) + '\n')
    report = {'schema_version': 1, 'status': 'draft_visual_review_required', 'manifest_sha256': sha(manifest_path),
              'visual_review_policy_sha256': sha(policy_path) if policy_path is not None else None,
              'detail_strength': detail_strength, 'edge_width_output_pixels': edge_width, 'target_scale': 4,
              'limitations': ['Metrics flag some color/layout changes but cannot certify correct text, silhouettes, semantics or new details.',
                              'Every replacement requires visual review. No generated image is automatically approved.',
                              'Joined images are corrected before splitting; the source manifest defines fragment boundaries.',
                              'Source edge strips preserve existing seams rather than inventing a new seamless surface.'],
              'plates': [{k: v for k, v in p.items() if k != 'image'} | {'id': key} for key, p in plate_records.items()],
              'summary': {'source_images': len(results), 'replacement_pngs': sum(len(r['regions']) for r in results),
                          'mapped_hashes': len(mappings), 'flagged_images': sum(bool(r['qa']['flags']) for r in results),
                          'protected_nb2_images': sum(r['qa']['candidate_status'] == 'protected_nb2_detail' for r in results),
                          'source_preserving_images': sum(r['qa']['candidate_status'] == 'source_preserving_4x' for r in results)},
              'images': results}
    (output / 'qa.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    prep = sub.add_parser('prepare')
    prep.add_argument('--inventory', required=True, type=Path)
    prep.add_argument('--output', required=True, type=Path)
    prep.add_argument('--grid', choices=('1x1', '2x1', '4x3', '8x6'), default='8x6')
    prep.add_argument('--tile-size', type=int)
    prep.add_argument('--border', type=int, default=8)
    prep.add_argument('--gutter', type=int, default=8)
    build = sub.add_parser('assemble')
    build.add_argument('--manifest', required=True, type=Path)
    build.add_argument('--generated', required=True, type=Path)
    build.add_argument('--output', required=True, type=Path)
    build.add_argument('--detail-strength', type=float, default=.75)
    build.add_argument('--edge-width', type=int, default=4)
    build.add_argument('--policy', type=Path, help='schema_version 1 JSON with per-image detail-strength limits and review reasons')
    args = parser.parse_args()
    try:
        if args.command == 'prepare':
            result = prepare(args.inventory, args.output, tuple(map(int, args.grid.split('x'))), args.tile_size, args.border, args.gutter)
            print(json.dumps({'plates': len(result['plates']), 'source_images': len(result['cells'])}))
            return 0
        result = assemble(args.manifest, args.generated, args.output, args.detail_strength, args.edge_width, args.policy)
        print(json.dumps(result['summary']))
        return 1 if result['summary']['flagged_images'] else 0
    except (OSError, ValueError, KeyError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    sys.exit(main())
