#!/usr/bin/env python3
"""Merge source inventories by decoded pixels and exact RT64 replacement keys."""
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inventory', type=Path, action='append', required=True)
    parser.add_argument('--exclude', type=Path, action='append', default=[])
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    excluded = {row['image_id'] for file in args.exclude
                for row in json.loads(file.read_text())['images']}
    images, textures, sources = {}, {}, []
    for path in args.inventory:
        path = path.resolve()
        data = json.loads(path.read_text())
        sources.append({'path': str(path), 'summary': data.get('summary', {})})
        for row in data['images']:
            ident = row['image_id']
            if ident in excluded:
                continue
            if ident not in images:
                images[ident] = dict(row, png=str((path.parent / row['png']).resolve()), hash_aliases=[], source_ids=[])
            for field in ['hash_aliases', 'source_ids']:
                images[ident][field] = sorted(set(images[ident][field]) | set(row.get(field, [])))
        for row in data['textures']:
            if row['image_id'] in excluded:
                continue
            key = row['hash']
            if key in textures and textures[key]['image_id'] != row['image_id']:
                raise ValueError(f'Conflicting decoded pixels for exact RT64 key {key}')
            if key not in textures:
                item = dict(row)
                for field in ['source_tile_json', 'source_tmem']:
                    if field in item:
                        item[field] = str((Path(data['input_directory']) / item[field]).resolve())
                textures[key] = item
    result = {'schema_version': 1, 'input_directory': '/', 'sources': sources,
              'textures': list(textures.values()), 'images': list(images.values()),
              'summary': {'unique_hashes': len(textures), 'unique_images': len(images),
                          'excluded_previously_processed_images': len(excluded)}, 'errors': []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result['summary']))


if __name__ == '__main__':
    main()
