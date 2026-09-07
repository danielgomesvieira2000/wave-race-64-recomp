#!/usr/bin/env python3
"""Join adjacent, compatible RT64 image strips before texture enhancement.

Every original replacement key retains an explicit crop rectangle. Contiguous
RDRAM addresses are only compared within one dump scene to avoid overlay reuse.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inventory', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    data = json.loads(args.inventory.read_text())
    source = args.inventory.resolve().parent
    dump = Path(data['input_directory'])
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / 'images').mkdir(exist_ok=True)
    images = {r['image_id']: dict(r, png=str((source / r['png']).resolve())) for r in data['images']}
    candidates = {}
    for record in data['textures']:
        if record['width'] < 64 or record['height'] > 16 or record['height'] < 2:
            continue
        path = dump / record['source_tile_json']
        rice = path.with_name(path.name.replace('.tile.json', '.rice.json'))
        if not rice.exists():
            continue
        meta = json.loads(rice.read_text())
        if meta['type'] != 'Block' or record['tlut'] != 'None':
            continue
        address = meta['texture']['address']
        size = (record['width'] * record['height'] * (4 << record['siz']) + 7) // 8
        key = (str(path.parent), record['width'], record['fmt'], record['siz'])
        candidates.setdefault(key, {})[address] = (record['image_id'], size)
    consumed, result, joined = set(), [], []
    for key, addresses in candidates.items():
        for address in sorted(addresses):
            ident, size = addresses[address]
            if ident in consumed:
                continue
            chain = [ident]
            cursor = address + size
            while cursor in addresses and len(chain) < 256:
                other, size = addresses[cursor]
                if other in chain or other in consumed:
                    break
                before = np.asarray(Image.open(images[chain[-1]]['png']).convert('RGBA'), dtype=np.float32)
                after = np.asarray(Image.open(images[other]['png']).convert('RGBA'), dtype=np.float32)
                # Respect both transparent silhouettes and premultiplied colour.
                edge_a = before[-1] * np.concatenate([before[-1, :, 3:4] / 255] * 3 + [np.ones((before.shape[1], 1))], axis=1)
                edge_b = after[0] * np.concatenate([after[0, :, 3:4] / 255] * 3 + [np.ones((after.shape[1], 1))], axis=1)
                if np.abs(edge_a - edge_b).mean() > 40:
                    break
                chain.append(other)
                cursor += size
            if len(chain) < 2:
                continue
            width = images[ident]['width']
            height = sum(images[i]['height'] for i in chain)
            combined = Image.new('RGBA', (width, height))
            regions, y = [], 0
            for i in chain:
                r = images[i]
                combined.paste(Image.open(r['png']).convert('RGBA'), (0, y))
                regions.append({'id': i, 'rect': [0, y, width, y + r['height']], 'hash_aliases': r['hash_aliases']})
                y += r['height']
            image_id = hashlib.sha256(combined.tobytes() + str(combined.size).encode()).hexdigest()
            dest = out / 'images' / (image_id + '.png')
            combined.save(dest)
            result.append({'image_id': image_id, 'png': str(dest), 'width': width, 'height': height,
                           'hash_aliases': [], 'regions': regions, 'source_images': chain,
                           'join_evidence': {'scene': key[0], 'start_address': address, 'end_address': cursor,
                                             'method': 'contiguous RDRAM loads with compatible format, width and adjacent pixels'}})
            consumed.update(chain)
            joined.append({'image_id': image_id, 'strips': len(chain), 'width': width, 'height': height})
    result.extend(r for i, r in images.items() if i not in consumed)
    data.update(images=result, joins=joined, source_inventory=str(args.inventory.resolve()))
    data['summary']['joined_images'] = len(joined)
    data['summary']['generation_images'] = len(result)
    (out / 'inventory.json').write_text(json.dumps(data, indent=2) + '\n')
    print(json.dumps({'joined_images': len(joined), 'strips_joined': len(consumed), 'generation_images': len(result)}))


if __name__ == '__main__':
    main()
