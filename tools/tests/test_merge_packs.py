"""Alias completion must retain reviewed pack precedence and source identity."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('merge_packs', ROOT / 'tools/textures/merge_packs.py')
merge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(merge)


def alias(number):
    return f'{number:016x}'


class MergePacksTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)
        self.output = self.directory / 'merged'

    def tearDown(self):
        self.temp.cleanup()

    def source(self, source_id, hashes, size=(4, 3), color=(20, 80, 100, 255)):
        path = self.directory / f'{source_id}-{len(list(self.directory.glob("*.png")))}.png'
        image = Image.new('RGBA', size, color)
        image.save(path)
        return {'image_id': source_id, 'png': str(path), 'width': size[0], 'height': size[1],
                'rgba_sha256': hashlib.sha256(image.tobytes()).hexdigest(),
                'hash_aliases': [alias(h) for h in hashes]}

    def inventory(self, name, images=(), updates=(), textures=()):
        path = self.directory / f'{name}.json'
        path.write_text(json.dumps({'images': list(images), 'base_hash_alias_updates': list(updates),
                                    'textures': list(textures)}))
        return path

    def pack(self, name, rows):
        path = self.directory / name
        path.mkdir()
        mappings = []
        for index, (hashes, size, color) in enumerate(rows):
            png = f'{index}.png'
            Image.new('RGBA', size, color).save(path / png)
            mappings.extend({'hashes': {'rt64': alias(h)}, 'path': png, 'operation': 'stream'} for h in hashes)
        (path / 'rt64.json').write_text(json.dumps({'configuration': {'hashVersion': 5}, 'textures': mappings}))
        return path

    def mappings(self):
        return {row['hashes']['rt64']: row for row in json.loads((self.output / 'rt64.json').read_text())['textures']}

    def test_repeat_inventory_and_deduplicated_addon_updates_complete_aliases(self):
        original = self.source('canonical', [1])
        update = dict(original, hash_aliases=[alias(2)])
        base = self.inventory('base', [original])
        addon = self.inventory('addon', updates=[update])
        pack = self.pack('base-pack', [([1], (16, 12), (30, 60, 100, 255))])
        result = subprocess.run([sys.executable, str(ROOT / 'tools/textures/merge_packs.py'),
                                 '--pack', str(pack), '--inventory', str(base), '--inventory', str(addon),
                                 '--output', str(self.output)], capture_output=True, text=True, check=True)
        self.assertEqual(json.loads(result.stdout)['active_hashes'], 2)
        rows = self.mappings()
        self.assertEqual(rows[alias(1)]['path'], rows[alias(2)]['path'])
        report = json.loads((self.output / 'pack.json').read_text())
        self.assertEqual(report['alias_completion']['canonical_source_images'], 1)
        self.assertEqual(report['inventories'][1]['base_hash_alias_updates'], 1)

    def test_joined_sources_match_region_ids_not_parent_id(self):
        left = self.source('left', [1, 2], color=(20, 80, 100, 255))
        right = self.source('right', [3, 4], color=(40, 90, 120, 255))
        parent = Image.new('RGBA', (8, 3))
        for i, record in enumerate((left, right)):
            with Image.open(record['png']) as image:
                parent.paste(image, (i * 4, 0))
        png = self.directory / 'parent.png'
        parent.save(png)
        joined = {'image_id': 'joined', 'png': str(png), 'width': 8, 'height': 3,
                  'regions': [{'id': 'left', 'rect': [0, 0, 4, 3], 'hash_aliases': [alias(1)]},
                              {'id': 'right', 'rect': [4, 0, 8, 3], 'hash_aliases': [alias(3)]}]}
        pack = self.pack('joined-pack', [([1], (16, 12), 'blue'), ([3], (16, 12), 'green')])
        report = merge.merge_packs([pack], self.output,
                                  [self.inventory('joined', [joined]), self.inventory('decoded', [left, right])])
        self.assertEqual(report['alias_completion']['canonical_source_images'], 2)
        self.assertEqual(report['alias_completion']['covered_source_images'], 2)
        self.assertEqual(report['active_hashes'], 4)
        rows = self.mappings()
        self.assertEqual(rows[alias(1)]['path'], rows[alias(2)]['path'])
        self.assertEqual(rows[alias(3)]['path'], rows[alias(4)]['path'])
        self.assertNotEqual(rows[alias(1)]['path'], rows[alias(3)]['path'])

    def test_latest_pack_supplies_missing_alias_but_explicit_aliases_are_preserved(self):
        inventory = self.inventory('sources', [self.source('one', [1, 2, 3])])
        earlier = self.pack('earlier', [([1, 2], (16, 12), 'blue')])
        later = self.pack('later', [([1], (16, 12), 'green')])
        report = merge.merge_packs([earlier, later], self.output, [inventory])
        rows = self.mappings()
        self.assertEqual(rows[alias(1)]['path'], rows[alias(3)]['path'])
        self.assertNotEqual(rows[alias(1)]['path'], rows[alias(2)]['path'])
        self.assertEqual(report['alias_completion']['added_aliases'][0]['pack_index'], 1)
        self.assertEqual(len(report['explicit_overrides']), 1)

    def test_equal_precedence_ambiguity_is_rejected_before_output(self):
        inventory = self.inventory('sources', [self.source('one', [1, 2, 3])])
        pack = self.pack('conflicting', [([1], (16, 12), 'blue'), ([2], (16, 12), 'green')])
        with self.assertRaisesRegex(ValueError, 'Ambiguous generated targets'):
            merge.merge_packs([pack], self.output, [inventory])
        self.assertFalse(self.output.exists())

    def test_explicit_8x_logo_override_and_alias_keep_world_4x(self):
        inventory = self.inventory('sources', [self.source('logo-strip', [1, 2]), self.source('world', [3])])
        base = self.pack('base', [([1], (16, 12), 'blue'), ([3], (16, 12), 'green')])
        logo = self.pack('approved-logo', [([1], (32, 24), 'red')])
        report = merge.merge_packs([base, logo], self.output, [inventory], extra_scales=(8,))
        rows = self.mappings()
        self.assertEqual(rows[alias(1)]['path'], rows[alias(2)]['path'])
        for key, expected in ((alias(1), (32, 24)), (alias(3), (16, 12))):
            with Image.open(self.output / rows[key]['path']) as image:
                self.assertEqual(image.size, expected)
        self.assertEqual(report['alias_completion']['target_scale'], 4)
        self.assertEqual(report['alias_completion']['allowed_target_scales'], [4, 8])
        self.assertEqual(report['alias_completion']['added_aliases'][0]['pack_index'], 1)
        self.assertEqual(len(report['explicit_overrides']), 1)
        # Exercise the actual opt-in CLI as well as its importable API.
        cli_output = self.directory / 'merged-cli'
        subprocess.run([sys.executable, str(ROOT / 'tools/textures/merge_packs.py'),
                        '--pack', str(base), '--pack', str(logo), '--inventory', str(inventory),
                        '--allow-scale', '8', '--output', str(cli_output)],
                       capture_output=True, text=True, check=True)
        self.assertEqual((cli_output / 'rt64.json').read_text(), (self.output / 'rt64.json').read_text())

    def test_8x_requires_explicit_approval(self):
        inventory = self.inventory('source', [self.source('logo', [1])])
        pack = self.pack('logo', [([1], (32, 24), 'red')])
        with self.assertRaisesRegex(ValueError, 'Target dimensions'):
            merge.merge_packs([pack], self.output, [inventory])
        self.assertFalse(self.output.exists())

    def test_approved_extra_scale_does_not_allow_mixed_axes_or_fractional_sizes(self):
        inventory = self.inventory('source', [self.source('logo', [1])])
        for size in ((32, 12), (16, 24), (31, 24), (24, 18)):
            with self.subTest(size=size):
                pack = self.pack(f'logo-{size[0]}-{size[1]}', [([1], size, 'red')])
                with self.assertRaisesRegex(ValueError, 'Target dimensions'):
                    merge.merge_packs([pack], self.output, [inventory], extra_scales=(8,))
                self.assertFalse(self.output.exists())

    def test_invalid_extra_scale_is_rejected(self):
        inventory = self.inventory('source', [self.source('logo', [1])])
        pack = self.pack('logo', [([1], (16, 12), 'red')])
        for scale in (0, -8, 8.0, True):
            with self.subTest(scale=scale):
                with self.assertRaisesRegex(ValueError, 'positive integers'):
                    merge.merge_packs([pack], self.output, [inventory], extra_scales=(scale,))
                self.assertFalse(self.output.exists())

    def test_source_target_and_observed_dimensions_are_validated(self):
        for problem in ('source', 'target', 'observed', 'region'):
            with self.subTest(problem=problem):
                record = self.source(f'source-{problem}', [1, 2])
                textures = []
                target_size = (16, 12)
                if problem == 'source':
                    record['width'] = 5
                elif problem == 'target':
                    target_size = (16, 11)
                elif problem == 'observed':
                    textures = [{'hash': alias(2), 'width': 3, 'height': 4}]
                else:
                    record['regions'] = [{'id': 'bad', 'rect': [0, 0, 5, 3], 'hash_aliases': [alias(1)]}]
                inventory = self.inventory(problem, [record], textures=textures)
                pack = self.pack(f'pack-{problem}', [([1], target_size, 'blue')])
                with self.assertRaisesRegex(ValueError, 'dimensions|Invalid source region'):
                    merge.merge_packs([pack], self.output, [inventory])
                self.assertFalse(self.output.exists())

    def test_source_identity_and_alias_conflicts_are_rejected(self):
        a = self.source('one', [1])
        b = self.source('one', [2], color=(2, 3, 4, 255))
        with self.assertRaisesRegex(ValueError, 'Conflicting source pixels'):
            merge.load_inventory_sources([self.inventory('conflicting-pixels', [a, b])])
        b['image_id'] = 'two'
        b['hash_aliases'] = [alias(1)]
        with self.assertRaisesRegex(ValueError, 'conflicting source IDs'):
            merge.load_inventory_sources([self.inventory('conflicting-alias', [a, b])])

    def test_unknown_targets_are_reported_without_inventing_replacements(self):
        pack = self.pack('partial', [([1], (16, 12), 'blue')])
        inventory = self.inventory('sources', [self.source('covered', [1, 2]), self.source('missing', [3])])
        report = merge.merge_packs([pack], self.output, [inventory])
        self.assertEqual(report['active_hashes'], 2)
        self.assertEqual(report['alias_completion']['covered_source_images'], 1)
        self.assertEqual(report['alias_completion']['sources_without_known_target'][0]['source_id'], 'missing')


if __name__ == '__main__':
    unittest.main()
