#!/usr/bin/env python3
"""Merge reviewed RT64 packs, with later packs explicitly overriding earlier keys."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

from PIL import Image


def rt64_hash(value):
    if not isinstance(value, str) or not re.fullmatch(r'[0-9a-fA-F]{16}', value):
        raise ValueError(f'Invalid RT64 hash: {value!r}')
    return value.lower()


def dimensions(width, height):
    if any(type(v) is not int or v <= 0 for v in (width, height)):
        raise ValueError(f'Invalid source dimensions: {(width, height)}')
    return width, height


def load_inventory_sources(paths):
    """Union decoded images and joined fragments by canonical source ID.

    Addon deduplication deliberately omits previously generated images from
    ``images``. Its full ``base_hash_alias_updates`` records are equally trusted
    source records and may introduce aliases absent from the original pack.
    """
    canonical, aliases, observed_dimensions, inventories = {}, {}, {}, []
    for inventory_path in paths:
        inventory_path = Path(inventory_path).resolve()
        blob = inventory_path.read_bytes()
        inventory = json.loads(blob)
        if inventory.get('errors'):
            raise ValueError(f'Inventory contains decoding errors: {inventory_path}')
        for texture in inventory.get('textures', []):
            metadata = texture.get('metadata', {})
            width = texture.get('width', metadata.get('width'))
            height = texture.get('height', metadata.get('height'))
            if width is not None or height is not None:
                key = rt64_hash(texture['hash'])
                size = dimensions(width, height)
                observed_dimensions.setdefault(key, set()).add(size)
                if 'width' in metadata and 'height' in metadata:
                    observed_dimensions[key].add(dimensions(metadata['width'], metadata['height']))
        images = inventory.get('images', [])
        updates = inventory.get('base_hash_alias_updates', [])
        if not isinstance(images, list) or not isinstance(updates, list):
            raise ValueError('images and base_hash_alias_updates must contain source records')
        for record in [*images, *updates]:
            path = inventory_path.parent / record['png']
            with Image.open(path) as opened:
                source = opened.convert('RGBA')
            size = dimensions(record['width'], record['height'])
            if source.size != size:
                raise ValueError(f'Source dimensions differ from inventory: {path}')
            source_digest = hashlib.sha256(source.tobytes()).hexdigest()
            if record.get('rgba_sha256') and source_digest != record['rgba_sha256']:
                raise ValueError(f'Source pixels differ from inventory: {path}')
            regions = record.get('regions') or [{
                'id': record['image_id'], 'rect': [0, 0, *size],
                'hash_aliases': record.get('hash_aliases', [])}]
            region_aliases = set()
            for region in regions:
                source_id = region.get('id')
                if not isinstance(source_id, str) or not source_id:
                    raise ValueError('Every source region needs its canonical source ID')
                rect = region.get('rect')
                if (not isinstance(rect, list) or len(rect) != 4
                        or any(type(v) is not int for v in rect)
                        or not 0 <= rect[0] < rect[2] <= size[0]
                        or not 0 <= rect[1] < rect[3] <= size[1]):
                    raise ValueError(f'Invalid source region for {source_id}: {rect!r}')
                fragment = source.crop(rect)
                original_size = region.get('original_size', list(fragment.size))
                if original_size != list(fragment.size):
                    raise ValueError(f'Inconsistent original_size for {source_id}')
                identity = (fragment.size, hashlib.sha256(fragment.tobytes()).hexdigest())
                if source_id in canonical and canonical[source_id]['identity'] != identity:
                    raise ValueError(f'Conflicting source pixels or dimensions for {source_id}')
                entry = canonical.setdefault(source_id, {'identity': identity, 'aliases': set()})
                hashes = region.get('hash_aliases', [])
                if not isinstance(hashes, list) or not hashes:
                    raise ValueError(f'Source {source_id} needs RT64 aliases')
                for value in hashes:
                    key = rt64_hash(value)
                    if key in aliases and aliases[key] != source_id:
                        raise ValueError(f'RT64 alias {key} identifies conflicting source IDs')
                    aliases[key] = source_id
                    entry['aliases'].add(key)
                    region_aliases.add(key)
            if record.get('regions') and not set(map(rt64_hash, record.get('hash_aliases', []))) <= region_aliases:
                raise ValueError(f'Joined image {record["image_id"]} has aliases outside its regions')
        inventories.append({'inventory': str(inventory_path), 'sha256': hashlib.sha256(blob).hexdigest(),
                            'images': len(images), 'base_hash_alias_updates': len(updates)})
    for key, source_id in aliases.items():
        expected = canonical[source_id]['identity'][0]
        observed = observed_dimensions.get(key, set())
        if observed and observed != {expected}:
            raise ValueError(f'RT64 alias {key} source dimensions {expected} disagree with {sorted(observed)}')
    return canonical, inventories


def complete_aliases(mappings, precedence, image_sizes, canonical, target_scale=4, extra_scales=()):
    """Fill only absent hashes; explicit mappings retain their authored precedence."""
    scales = [target_scale, *extra_scales]
    if any(type(scale) is not int or scale < 1 for scale in scales):
        raise ValueError('target-scale and extra scales must be positive integers')
    scales = sorted(set(scales))
    additions, missing_sources, covered_sources = [], [], 0
    for source_id, source in sorted(canonical.items()):
        existing = sorted(source['aliases'].intersection(mappings))
        # Additional approved scales admit entire uniformly scaled images, not
        # independent per-axis choices (e.g. 8x width with 4x height).
        expected = [tuple(v * scale for v in source['identity'][0]) for scale in scales]
        for key in existing:
            actual = image_sizes[mappings[key]['path']]
            if actual not in expected:
                raise ValueError(f'Target dimensions for {source_id}/{key} are {actual}; expected {expected}')
        if not existing:
            missing_sources.append({'source_id': source_id, 'hash_aliases': sorted(source['aliases'])})
            continue
        covered_sources += 1
        latest = max(precedence[key] for key in existing)
        candidates = [key for key in existing if precedence[key] == latest]
        # Non-hash settings (operation, shifts, etc.) must also agree. A matching
        # PNG alone does not establish which conflicting sampler rule to copy.
        identities = {json.dumps({k: v for k, v in mappings[key].items() if k != 'hashes'}, sort_keys=True)
                      for key in candidates}
        if len(identities) != 1:
            raise ValueError(f'Ambiguous generated targets in the same pack for {source_id}: {candidates}')
        anchor = candidates[0]
        for key in sorted(source['aliases'].difference(mappings)):
            # Other hash families identify the original alias and cannot safely
            # be carried onto an RT64-only alias inferred from this inventory.
            mappings[key] = dict(mappings[anchor], hashes={'rt64': key})
            precedence[key] = latest
            additions.append({'hash': key, 'source_id': source_id, 'anchor_hash': anchor,
                              'path': mappings[anchor]['path'], 'pack_index': latest})
    return {'canonical_source_images': len(canonical), 'covered_source_images': covered_sources,
            'inventory_aliases': sum(len(s['aliases']) for s in canonical.values()),
            'added_aliases': additions, 'sources_without_known_target': missing_sources,
            'target_scale': target_scale, 'allowed_target_scales': scales}


def merge_packs(packs, output, inventories=(), target_scale=4, extra_scales=()):
    output = Path(output).resolve()
    canonical, inventory_reports = load_inventory_sources(inventories)
    mappings, sources, overrides = {}, [], []
    image_sizes, image_files, precedence = {}, {}, {}
    config = None
    for pack_index, source in enumerate(packs):
        source = Path(source).resolve()
        data = json.loads((source / 'rt64.json').read_text())
        current = data['configuration']
        if config is not None and config != current:
            raise ValueError('Incompatible replacement configurations')
        config = current
        count = 0
        for mapping in data['textures']:
            key = rt64_hash(mapping['hashes']['rt64'])
            path = (source / mapping['path']).resolve()
            if not path.is_relative_to(source):
                raise ValueError('Pack image escapes its directory')
            blob = path.read_bytes()
            digest = hashlib.sha256(blob).hexdigest()
            with Image.open(path) as image:
                if image.format != 'PNG':
                    raise ValueError(f'Merge input must be a PNG pack: {path}')
                size = image.size
                image.verify()
            relative = 'textures/' + digest + '.png'
            image_sizes[relative] = size
            image_files[relative] = path
            mapped = dict(mapping, hashes=dict(mapping['hashes'], rt64=key), path=relative)
            if key in mappings and precedence[key] == pack_index and mappings[key] != mapped:
                raise ValueError(f'Conflicting duplicate RT64 alias {key} in {source}')
            if key in mappings and mappings[key] != mapped:
                overrides.append({'hash': key, 'later_pack': str(source)})
            mappings[key] = mapped
            precedence[key] = pack_index
            count += 1
        sources.append({'pack': str(source), 'mappings': count,
                        'database_sha256': hashlib.sha256((source / 'rt64.json').read_bytes()).hexdigest()})
    if config is None:
        raise ValueError('At least one pack is required')
    alias_report = complete_aliases(mappings, precedence, image_sizes, canonical, target_scale, extra_scales)
    # Complete validation before writing any part of the merged database.
    output.mkdir(parents=True, exist_ok=True)
    (output / 'textures').mkdir(exist_ok=True)
    used = {mapping['path'] for mapping in mappings.values()}
    for relative in sorted(used):
        target = output / relative
        if target.exists():
            if hashlib.sha256(target.read_bytes()).hexdigest() != Path(relative).stem:
                raise ValueError(f'Existing content-addressed image was modified: {target}')
        else:
            shutil.copyfile(image_files[relative], target)
    database = {'configuration': config, 'textures': [mappings[key] for key in sorted(mappings)]}
    (output / 'rt64.json').write_text(json.dumps(database, indent=2) + '\n')
    # Retain any old files: database membership, not directory contents, defines
    # active replacements. A later rebuild can be staged into a fresh directory.
    report = {'status': 'local_pack_pending_native_review', 'sources': sources,
              'inventories': inventory_reports, 'alias_completion': alias_report,
              'active_hashes': len(mappings), 'active_pngs': len(used),
              'explicit_overrides': overrides,
              'active_png_bytes': sum((output / path).stat().st_size for path in used)}
    (output / 'pack.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pack', type=Path, action='append', required=True)
    parser.add_argument('--inventory', type=Path, action='append', default=[],
                        help='Union canonical source aliases, including addon base_hash_alias_updates; repeat as needed')
    parser.add_argument('--target-scale', type=int, default=4,
                        help='Primary allowed uniform scale for inventory-matched replacements (default: 4)')
    parser.add_argument('--allow-scale', type=int, action='append', default=[],
                        help='Explicitly approve an additional uniform integer scale (e.g. 8 for reviewed logos); repeat as needed')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = merge_packs(args.pack, args.output, args.inventory, args.target_scale, args.allow_scale)
    print(json.dumps({key: report[key] for key in ('active_hashes', 'active_pngs', 'active_png_bytes')}))


if __name__ == '__main__':
    main()
