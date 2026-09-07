#!/usr/bin/env python3
"""Build audited 4x font replacements from source contours and reviewed NB2 art.

Source fonts reconstruct alpha and fill independently. Explicitly supplied NB2
font manifests use a clean matte and retain each source word's placement bounds;
reports distinguish this redraw from source contour reconstruction.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re

import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

SCALE = 4
FONT_CANVAS_GUARD = 2  # Half a native texel: keep clamp-filter taps off glyph ink.


def safe_ink_bounds(field: np.ndarray, threshold: float = .5, guard: int = FONT_CANVAS_GUARD) -> list[int]:
    """Place crisp ink inside source coverage, leaving transparent sampler taps.

The source's faint antialiasing fringe is not an opaque glyph boundary. Fitting
to that fringe expands the new letters, and nonzero canvas-edge texels are
repeated by clamped texture sampling. The output canvas itself is never changed.
    """
    maximum = float(field.max())
    if not np.isfinite(maximum) or maximum <= 0:
        raise ValueError('font source has no placement bounds')
    ink = np.argwhere(field >= min(threshold, maximum * .75))
    if not len(ink):
        raise ValueError('font source has no placement bounds')
    bounds = [int(ink[:, 1].min()), int(ink[:, 0].min()), int(ink[:, 1].max() + 1), int(ink[:, 0].max() + 1)]
    h, w = field.shape
    bounds = [max(guard, bounds[0]), max(guard, bounds[1]), min(w - guard, bounds[2]), min(h - guard, bounds[3])]
    if bounds[0] >= bounds[2] or bounds[1] >= bounds[3]:
        raise ValueError('font placement is smaller than its sampling guard')
    return bounds


def guard_font_canvas(image: Image.Image) -> tuple[Image.Image, dict]:
    """Keep reconstructed source glyphs off the clamp edge without cropping them."""
    source_bounds = image.getchannel('A').getbbox()
    if source_bounds is None:
        return image, {'adjusted': False}
    guard = FONT_CANVAS_GUARD
    left, top, right, bottom = source_bounds
    width, height = min(right - left, image.width - guard * 2), min(bottom - top, image.height - guard * 2)
    x = max(guard, min(left, image.width - guard - width))
    y = max(guard, min(top, image.height - guard - height))
    target_bounds = [x, y, x + width, y + height]
    if target_bounds == list(source_bounds):
        return image, {'adjusted': False, 'source_ink_bounds': list(source_bounds), 'target_ink_bounds': target_bounds}
    result = Image.new('RGBA', image.size)
    glyph = image.crop(source_bounds).convert('RGBa').resize((width, height), Image.Resampling.LANCZOS).convert('RGBA')
    result.paste(glyph, (x, y))
    return result, {'adjusted': True, 'source_ink_bounds': list(source_bounds), 'target_ink_bounds': target_bounds}


def resize_field(field: np.ndarray, size: tuple[int, int], resample=Image.Resampling.BICUBIC) -> np.ndarray:
    return np.asarray(Image.fromarray(field.astype(np.float32)).resize(size, resample), dtype=np.float32)


def contour_coverage(field: np.ndarray, size: tuple[int, int], threshold=.5, supersample=4) -> np.ndarray:
    """Integrate a smooth source isocontour over each output pixel, not a blur."""
    padded = np.pad(field, 2, mode='constant')
    factor_x, factor_y = size[0] / field.shape[1], size[1] / field.shape[0]
    high_size = (round((field.shape[1] + 4) * factor_x * supersample),
                 round((field.shape[0] + 4) * factor_y * supersample))
    interpolated = resize_field(padded, high_size)
    hard = Image.fromarray((interpolated >= threshold).astype(np.uint8) * 255)
    left, top = round(2 * factor_x * supersample), round(2 * factor_y * supersample)
    hard = hard.crop((left, top, left + size[0] * supersample, top + size[1] * supersample))
    return np.asarray(hard.resize(size, Image.Resampling.BOX), dtype=np.float32) / 255


def refine_font(image: Image.Image, family: str | None = None, fill_threshold: float | None = None) -> tuple[Image.Image, dict]:
    original = image.convert('RGBA')
    source = np.asarray(original, dtype=np.float32) / 255
    rgb, alpha = source[..., :3], source[..., 3]
    visible = alpha > .5
    if not np.any(visible):
        raise ValueError('font has no visible source coverage')
    maximum = np.max(rgb, axis=2)
    gray = np.max(np.ptp(rgb[visible], axis=1)) < 2 / 255
    if family is None:
        family = 'plain-white' if gray and np.min(maximum[visible]) >= .75 else ('outlined-white' if gray else 'colored-hud')
    if family not in ('plain-white', 'outlined-white', 'colored-hud'):
        raise ValueError(f'unknown font family {family}')
    size = original.width * SCALE, original.height * SCALE
    out_alpha = contour_coverage(alpha, size)
    if family == 'plain-white':
        # IA words store antialiasing in alpha; RGB remains their source white.
        weight = np.maximum(resize_field(alpha, size), 1e-6)
        out_rgb = np.stack([resize_field(rgb[..., c] * alpha, size) / weight for c in range(3)], axis=2)
        fill = out_alpha
    else:
        # Black outline/shadow and colored/white fill have separate isocontours.
        # Recover the hue field independently so sharpening cannot move the HUD's
        # original yellow-green/red vertical gradient or introduce new colors.
        fill_native = maximum * alpha
        if fill_threshold is None:
            fill_threshold = .60 if family == 'colored-hud' and original.width >= 24 and original.height <= 12 else .50
        # Low-intensity punctuation still has a real contour; do not erase it by
        # imposing the white-letter threshold on a dim source punctuation mark.
        effective_threshold = min(fill_threshold, float(fill_native.max()) * .65)
        fill = np.minimum(contour_coverage(fill_native, size, effective_threshold), out_alpha)
        chroma_weight = np.maximum(resize_field(fill_native, size), 1e-6)
        chroma = np.stack([resize_field(rgb[..., c] * alpha, size) / chroma_weight for c in range(3)], axis=2)
        bevel = .35 + .65 * np.clip(resize_field(maximum, size), 0, 1)
        out_rgb = chroma * bevel[..., None] * np.divide(fill, out_alpha, out=np.zeros_like(fill), where=out_alpha > 0)[..., None]
        # Preserve a small amount of the original black drop shadow outside the
        # reconstructed opaque outline, without broadening the bright fill.
        out_alpha = np.maximum(out_alpha, np.clip(resize_field(alpha, size), 0, 1) * .12)
    out_rgb = np.clip(out_rgb, 0, 1)
    out_rgb[out_alpha == 0] = 0
    result = np.dstack([np.rint(out_rgb * 255).astype(np.uint8), np.rint(out_alpha * 255).astype(np.uint8)])
    guarded, canvas_adjustment = guard_font_canvas(Image.fromarray(result))
    result = np.asarray(guarded)
    out_alpha = result[..., 3].astype(np.float32) / 255
    report = {'family': family, 'source_size': list(original.size), 'output_size': list(size),
              'method': 'source-isocontour supersampling with independent alpha, fill and chroma fields',
              'scale': SCALE, 'contour_threshold': .5, 'coverage_supersampling': 4,
              'fill_threshold': None if family == 'plain-white' else effective_threshold,
              'source_rgba_sha256': hashlib.sha256(np.asarray(original).tobytes()).hexdigest(),
              'alpha_partial_pixels': int(np.count_nonzero((result[..., 3] > 0) & (result[..., 3] < 255))),
              'source_alpha_coverage': float(alpha.sum()), 'output_alpha_coverage_in_source_pixels': float(out_alpha.sum() / SCALE ** 2),
              'canvas_guard_output_pixels': FONT_CANVAS_GUARD, 'canvas_adjustment': canvas_adjustment,
              'review_status': 'draft_visual_review_required'}
    return guarded, report


def extract_nb2_font(plate: Image.Image, cell: dict, source: Image.Image,
                     reference_size: tuple[int, int] = (1024, 768)) -> tuple[Image.Image, dict]:
    """Matte a reviewed whole-word/glyph redraw and retain its source placement."""
    plate = plate.convert('RGB')
    if min(reference_size) <= 0:
        raise ValueError('invalid reference plate size')
    rect = [round(v * (plate.width / reference_size[0] if i % 2 == 0 else plate.height / reference_size[1]))
            for i, v in enumerate(cell['cell_rect'])]
    if len(rect) != 4 or not 0 <= rect[0] < rect[2] <= plate.width or not 0 <= rect[1] < rect[3] <= plate.height:
        raise ValueError('invalid generated font cell rectangle')
    tile = np.asarray(plate.crop(rect), dtype=np.float32) / 255
    border = np.concatenate([tile[:8].reshape(-1, 3), tile[-8:].reshape(-1, 3),
                             tile[:, :8].reshape(-1, 3), tile[:, -8:].reshape(-1, 3)])
    background = np.median(border, axis=0)
    difference = np.max(np.abs(tile - background), axis=2)
    alpha = np.clip((difference - 12 / 255) / (12 / 255), 0, 1)
    foreground = np.clip((tile - background * (1 - alpha[..., None])) / np.maximum(alpha[..., None], 1e-5), 0, 1)
    rgba = np.dstack([np.rint(foreground * 255).astype(np.uint8), np.rint(alpha * 255).astype(np.uint8)])
    glyph = Image.fromarray(rgba)
    ink = np.argwhere(alpha > .5)
    if not len(ink):
        raise ValueError(f'generated font cell {cell["image_id"]} contains no matte foreground')
    generated_bounds = [int(ink[:, 1].min()), int(ink[:, 0].min()), int(ink[:, 1].max() + 1), int(ink[:, 0].max() + 1)]
    # Use source coverage only for its bounds. Reapplying its soft alpha would
    # blur or clip the carefully redrawn lettering back into the original shape.
    source = source.convert('RGBA')
    target_size = source.width * SCALE, source.height * SCALE
    trusted_alpha = np.asarray(source.getchannel('A').resize(target_size, Image.Resampling.BICUBIC)) / 255
    target_bounds = safe_ink_bounds(trusted_alpha)
    target = Image.new('RGBA', target_size)
    size = (target_bounds[2] - target_bounds[0], target_bounds[3] - target_bounds[1])
    cropped = glyph.crop(generated_bounds).convert('RGBa').resize(size, Image.Resampling.LANCZOS).convert('RGBA')
    target.paste(cropped, (target_bounds[0], target_bounds[1]))
    return target, {'method': 'Nano Banana 2 font redraw, gray-matte removal and source-bound placement',
                    'source_size': list(source.size), 'output_size': list(target_size), 'scale': SCALE,
                    'source_rgba_sha256': hashlib.sha256(np.asarray(source).tobytes()).hexdigest(),
                    'sampled_cell_rect': rect, 'generated_ink_bounds': generated_bounds,
                    'target_source_ink_bounds': target_bounds, 'matte_rgb': np.rint(background * 255).astype(int).tolist(),
                    'canvas_guard_output_pixels': FONT_CANVAS_GUARD, 'placement_alpha_threshold': .5,
                    'review_status': 'draft_native_review_required'}


def vector_font(source: Image.Image, text: str, font_path: Path, font_index: int = 0,
                outlined: bool = False) -> tuple[Image.Image, dict]:
    """Render an audited transcription using a fitted, source-matched vector font.

The texture dimensions are retained and crisp ink is fitted inside the source's
coverage with a transparent sampling guard. Where the source separates into the
known glyph count, placement candidates retain its spacing. Native-coverage
comparison selects the closest valid placement, never the text.
    """
    if not text or '\n' in text or not text.strip():
        raise ValueError('vector font requires one audited nonempty line')
    source = source.convert('RGBA')
    size = (source.width * SCALE, source.height * SCALE)
    rgba = np.asarray(source, dtype=np.float32) / 255
    original_alpha = rgba[..., 3]
    source_alpha = original_alpha * rgba[..., :3].max(axis=2) if outlined else original_alpha
    high_alpha = np.clip(resize_field(source_alpha, size), 0, 1)
    bounds = safe_ink_bounds(high_alpha)
    font = ImageFont.truetype(str(font_path), 256, index=font_index)

    def glyph_alpha(word):
        left, top, right, bottom = font.getbbox(word)
        canvas = Image.new('L', (right - left + 8, bottom - top + 8))
        ImageDraw.Draw(canvas).text((4 - left, 4 - top), word, fill=255, font=font)
        bbox = canvas.getbbox()
        if bbox is None:
            raise ValueError('vector font cannot render audited text')
        return canvas.crop(bbox)

    def place(alpha, rect, canvas):
        alpha = alpha.resize((rect[2] - rect[0], rect[3] - rect[1]), Image.Resampling.LANCZOS)
        canvas.paste(alpha, (rect[0], rect[1]))

    def error(alpha):
        native = np.asarray(alpha.resize(source.size, Image.Resampling.BOX), dtype=np.float32) / 255
        return float(np.mean((native - source_alpha) ** 2))

    whole = Image.new('L', size)
    place(glyph_alpha(text), bounds, whole)
    best, best_error, placements = whole, error(whole), [{'text': text, 'bounds': bounds}]
    chars = [c for c in text if not c.isspace()]
    glyphs = [glyph_alpha(c) for c in chars]
    profile = high_alpha.max(axis=0)
    for threshold in (.35, .4, .45, .5, .55, .6, .65):
        edges = np.diff(np.pad((profile >= threshold).astype(np.int8), (1, 1)))
        starts, ends = np.flatnonzero(edges == 1), np.flatnonzero(edges == -1)
        if len(starts) != len(chars):
            continue
        cuts = [0] + [int((ends[i] + starts[i + 1]) // 2) for i in range(len(chars) - 1)] + [size[0]]
        candidate, regions = Image.new('L', size), []
        for i, char in enumerate(chars):
            pixels = np.argwhere(high_alpha[:, cuts[i]:cuts[i + 1]] >= min(.5, float(high_alpha.max()) * .75))
            if not len(pixels):
                break
            rect = [int(pixels[:, 1].min()) + cuts[i], int(pixels[:, 0].min()),
                    int(pixels[:, 1].max() + 1) + cuts[i], int(pixels[:, 0].max() + 1)]
            rect = [max(bounds[0], rect[0]), max(bounds[1], rect[1]), min(bounds[2], rect[2]), min(bounds[3], rect[3])]
            if rect[0] >= rect[2] or rect[1] >= rect[3]:
                break
            place(glyphs[i], rect, candidate)
            regions.append({'text': char, 'bounds': rect})
        else:
            score = error(candidate)
            if score < best_error:
                best, best_error, placements = candidate, score, regions
    # These audited words use a uniform white/tint. Sampling the old alpha under
    # a new stroke would paint black holes where a clean contour crosses an old
    # transparent pixel, so recover the visible source ink color independently.
    color_pixels = rgba[..., :3][source_alpha >= float(source_alpha.max()) * .5]
    tint = (np.max if outlined else np.median)(color_pixels, axis=0)
    rgb = np.broadcast_to(tint, (size[1], size[0], 3)).copy()
    alpha = np.asarray(best)
    outline_radius = None
    if outlined:
        original_high_alpha = np.clip(resize_field(original_alpha, size), 0, 1)
        original_ink = np.argwhere(original_high_alpha >= .25)
        outer = [max(FONT_CANVAS_GUARD, int(original_ink[:, 1].min())), max(FONT_CANVAS_GUARD, int(original_ink[:, 0].min())),
                 min(size[0] - FONT_CANVAS_GUARD, int(original_ink[:, 1].max() + 1)),
                 min(size[1] - FONT_CANVAS_GUARD, int(original_ink[:, 0].max() + 1))]
        # Match the source's measured outline width, not an arbitrary bold stroke.
        margins = [bounds[0] - outer[0], bounds[1] - outer[1], outer[2] - bounds[2], outer[3] - bounds[3]]
        outline_radius = float(np.clip(np.median(margins), .5, SCALE))
        high = best.resize((size[0] * 4, size[1] * 4), Image.Resampling.LANCZOS)
        thick = high.filter(ImageFilter.MaxFilter(round(outline_radius * 4) * 2 + 1))
        outline = np.asarray(thick.resize(size, Image.Resampling.LANCZOS), dtype=np.float32) / 255
        # Retain the source's occupied canvas bounds without clipping letter ink.
        extent = np.zeros_like(outline)
        extent[outer[1]:outer[3], outer[0]:outer[2]] = 1
        outline *= extent
        fill = alpha.astype(np.float32) / 255
        outline = np.maximum(outline, fill)
        rgb *= np.divide(fill, outline, out=np.zeros_like(fill), where=outline > 0)[..., None]
        alpha = np.rint(outline * 255).astype(np.uint8)
    rgb = np.rint(np.clip(rgb, 0, 1) * 255).astype(np.uint8)
    rgb[alpha == 0] = 0
    result = Image.fromarray(np.dstack([rgb, alpha]))
    return result, {'method': 'audited text reconstructed with source-matched vector font and source-bound registration',
                    'text': text, 'font_family': list(font.getname()), 'font_file': str(font_path.resolve()),
                    'font_file_sha256': hashlib.sha256(font_path.read_bytes()).hexdigest(),
                    'font_index': font_index, 'source_size': list(source.size), 'output_size': list(size), 'scale': SCALE,
                    'source_rgba_sha256': hashlib.sha256(np.asarray(source).tobytes()).hexdigest(),
                    'glyph_placements': placements, 'native_coverage_rmse': best_error ** .5,
                    'outlined': outlined, 'outline_radius_output_pixels': outline_radius,
                    'source_ink_rgb': np.rint(tint * 255).astype(int).tolist(),
                    'canvas_guard_output_pixels': FONT_CANVAS_GUARD, 'placement_alpha_threshold': .5,
                    'review_status': 'draft_native_review_required'}


def build_pack(catalog_path: Path, output: Path, nb2_manifests: list[Path] | None = None,
               vector_font_path: Path | None = None, vector_font_index: int = 0) -> dict:
    catalog = json.loads(catalog_path.read_text())
    if catalog.get('schema_version') != 1:
        raise ValueError('unsupported font catalog')
    output.mkdir(parents=True, exist_ok=True)
    (output / 'textures').mkdir(exist_ok=True)
    candidates = {}
    for manifest_path in nb2_manifests or []:
        manifest = json.loads(manifest_path.read_text())
        plates = {p['id']: (Image.open(manifest_path.parent / 'generated' / (p['id'] + '.png')).convert('RGB'), tuple(p['size']))
                  for p in manifest['plates']}
        for cell in manifest['cells']:
            if cell['image_id'] in candidates:
                raise ValueError('duplicate NB2 font candidate')
            candidates[cell['image_id']] = (cell, *plates[cell['plate_id']], manifest_path)
    mappings, reports = {}, []
    for record in catalog['images']:
        source_path = catalog_path.parent / record['png']
        with Image.open(source_path) as opened:
            source = opened.convert('RGBA')
        digest = hashlib.sha256(np.asarray(source).tobytes()).hexdigest()
        if digest != record['rgba_sha256'] or source.size != (record['width'], record['height']):
            raise ValueError(f'source content/dimensions changed for {record["image_id"]}')
        full_source = source
        visible_height = record.get('visible_art_height', source.height)
        if type(visible_height) is not int or not 0 < visible_height <= source.height:
            raise ValueError('invalid visible font art height')
        preserved_rows = record.get('preserve_original_rows', [])
        if preserved_rows != list(range(visible_height, source.height)):
            raise ValueError('every row outside visible font art must be explicitly preserved')
        # Some original overlapping texture transfers read one row of the next
        # asset. It is sampler data, not part of the glyph or its placement box.
        source = source.crop((0, 0, source.width, visible_height))
        render_digest = hashlib.sha256(source.tobytes()).hexdigest()
        if record['image_id'] in candidates:
            cell, plate, reference_size, manifest_path = candidates[record['image_id']]
            if cell['source_rgba_sha256'] != render_digest:
                raise ValueError('reviewed NB2 reference differs from trusted font source')
            if record.get('nb2_sample_rect'):
                # A reviewed cell may contain an unwanted copied style reference.
                # Explicit catalog crops isolate the intended word and remain
                # separate from immutable generation/crop provenance.
                cell = {**cell, 'cell_rect': record['nb2_sample_rect']}
            result, qa = extract_nb2_font(plate, cell, source, reference_size)
            qa['candidate_manifest_sha256'] = hashlib.sha256(manifest_path.read_bytes()).hexdigest()
            qa['candidate_plate_sha256'] = hashlib.sha256((manifest_path.parent / 'generated' / (cell['plate_id'] + '.png')).read_bytes()).hexdigest()
        elif record.get('source_preserving'):
            result = source.resize((source.width * SCALE, source.height * SCALE), Image.Resampling.LANCZOS)
            qa = {'method': 'source-preserving 4x resample of auxiliary icon',
                  'source_size': list(source.size), 'output_size': list(result.size), 'scale': SCALE,
                  'source_rgba_sha256': digest, 'review_status': 'draft_native_review_required'}
        elif record.get('vector_parts'):
            if vector_font_path is None:
                raise ValueError('audited vector text requires --vector-font')
            result = source.resize((source.width * SCALE, source.height * SCALE), Image.Resampling.LANCZOS)
            parts = []
            for part in record['vector_parts']:
                rect = part['rect']
                if len(rect) != 4 or not 0 <= rect[0] < rect[2] <= source.width or not 0 <= rect[1] < rect[3] <= source.height:
                    raise ValueError('invalid font text-part rectangle')
                rendered, part_report = vector_font(source.crop(rect), part['text'], vector_font_path, vector_font_index)
                result.paste(rendered, (rect[0] * SCALE, rect[1] * SCALE))
                parts.append({'source_rect': rect, 'qa': part_report})
            qa = {'method': 'audited vector text regions with source-preserved surrounding content',
                  'source_size': list(source.size), 'output_size': list(result.size), 'scale': SCALE,
                  'source_rgba_sha256': digest, 'vector_parts': parts, 'review_status': 'draft_native_review_required'}
        elif record.get('vector_text') is not None:
            if vector_font_path is None:
                raise ValueError('audited vector text requires --vector-font')
            result, qa = vector_font(source, record['vector_text'], vector_font_path, vector_font_index, record.get('vector_outline', False))
        else:
            result, qa = refine_font(source, record.get('family'), record.get('fill_threshold'))
        if preserved_rows:
            # Exact texel replication keeps the deliberate overread independent
            # of the restored lettering and avoids filtering across asset rows.
            restored = full_source.resize((full_source.width * SCALE, full_source.height * SCALE), Image.Resampling.NEAREST)
            restored.paste(result, (0, 0))
            result = restored
            qa.update({'render_source_size': list(source.size),
                       'render_source_rgba_sha256': render_digest,
                       'source_size': list(full_source.size),
                       'source_rgba_sha256': digest, 'output_size': list(result.size),
                       'visible_art_height': visible_height,
                       'preserved_original_rows': preserved_rows,
                       'preserved_rows_method': 'exact 4x original texel replication; excluded from glyph fitting',
                       'canvas_guard_scope': 'visible artwork only; explicit overread rows retain original samples'})
        source = full_source
        regions = record.get('regions') or [{'id': record['image_id'], 'rect': [0, 0, source.width, source.height],
                                             'hash_aliases': record['hash_aliases']}]
        outputs = []
        for region in regions:
            rect = region['rect']
            if len(rect) != 4 or not 0 <= rect[0] < rect[2] <= source.width or not 0 <= rect[1] < rect[3] <= source.height:
                raise ValueError('invalid font source fragment')
            fragment = result.crop(tuple(v * SCALE for v in rect))
            name = hashlib.sha256((record['image_id'] + json.dumps(rect)).encode()).hexdigest() + '.png'
            path = 'textures/' + name
            fragment.save(output / path)
            for alias in region['hash_aliases']:
                if not re.fullmatch('[0-9a-f]{16}', alias):
                    raise ValueError('font RT64 hash must contain 16 lowercase hex digits')
                if alias in mappings and mappings[alias] != path:
                    raise ValueError(f'conflicting font replacement for {alias}')
                mappings[alias] = path
            outputs.append({'path': path, 'source_rect': rect, 'output_size': list(fragment.size), 'hash_aliases': region['hash_aliases'],
                            'png_sha256': hashlib.sha256((output / path).read_bytes()).hexdigest()})
        reports.append({'image_id': record['image_id'], 'label': record.get('label'),
                        'source_inventories': record.get('source_inventories', []),
                        'join_provenance': record.get('join_provenance'),
                        'transcription_proof': record.get('transcription_proof', []),
                        'qa': qa, 'regions': outputs})
    config = {'autoPath': 'rt64', 'configurationVersion': 3, 'hashVersion': 5, 'defaultOperation': 'stream', 'defaultShift': 'half'}
    database = {'configuration': config, 'textures': [{'hashes': {'rt64': key}, 'path': value} for key, value in sorted(mappings.items())]}
    (output / 'rt64.json').write_text(json.dumps(database, indent=2) + '\n')
    report = {'schema_version': 1, 'status': 'draft_native_review_required', 'catalog_sha256': hashlib.sha256(catalog_path.read_bytes()).hexdigest(),
              'output_content_sha256': hashlib.sha256(json.dumps(sorted((reg['path'], reg['png_sha256']) for row in reports for reg in row['regions'])).encode()).hexdigest(),
              'summary': {'font_images': len(reports), 'mapped_hashes': len(mappings), 'nb2_font_candidates': sum(r['image_id'] in candidates for r in reports),
                          'vector_fonts': sum('glyph_placements' in r['qa'] or 'vector_parts' in r['qa'] for r in reports)},
              'images': reports}
    (output / 'font-report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--catalog', type=Path)
    parser.add_argument('--nb2-manifest', action='append', type=Path, default=[])
    parser.add_argument('--vector-font', type=Path, help='Source-matched local font for explicitly transcribed catalog records')
    parser.add_argument('--vector-font-index', type=int, default=0)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--family', choices=('plain-white', 'outlined-white', 'colored-hud'))
    args = parser.parse_args()
    if args.catalog:
        report = build_pack(args.catalog, args.output, args.nb2_manifest, args.vector_font, args.vector_font_index)
        print(json.dumps(report['summary']))
        return 0
    if args.source is None:
        parser.error('provide --source or --catalog')
    with Image.open(args.source) as image:
        result, report = refine_font(image, args.family)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    result.save(args.output)
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
