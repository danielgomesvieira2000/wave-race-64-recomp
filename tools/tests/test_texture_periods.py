"""Scrolling phases share precisely the same upscaled texels across wraparound."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('atlas', ROOT / 'tools/textures/atlas.py')
atlas = importlib.util.module_from_spec(spec)
spec.loader.exec_module(atlas)


class PeriodTest(unittest.TestCase):
    def setUp(self):
        build = ROOT / 'build/texture-atlas-tests'
        build.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=build)
        self.root = Path(self.temp.name)
        y, x = np.indices((8, 12))
        pixels = np.stack((x * 15 + 20, y * 20 + 30, (x + y) * 8 + 20, x * 0 + 255), axis=2).astype('uint8')
        self.pixels = np.tile(pixels, (2, 1, 1))
        Image.fromarray(self.pixels).save(self.root / 'source.png')
        self.record = {'image_id': 'parent', 'png': 'source.png', 'width': 12, 'height': 16,
                       'rgba_sha256': hashlib.sha256(self.pixels.tobytes()).hexdigest(), 'periodic_y': 8,
                       'regions': [{'id': 'phase0', 'rect': [0, 0, 12, 8], 'hash_aliases': ['0000000000000001']},
                                   {'id': 'phase1', 'rect': [0, 8, 12, 16], 'hash_aliases': ['0000000000000002']}]}

    def tearDown(self):
        self.temp.cleanup()

    def run_assembly(self, grid):
        inventory = self.root / 'inventory.json'
        inventory.write_text(json.dumps({'images': [self.record]}))
        prepared = self.root / ('prepared-' + str(grid[0]))
        manifest = atlas.prepare(inventory, prepared, grid=grid)
        generated = self.root / ('generated-' + str(grid[0]))
        generated.mkdir()
        for plate in manifest['plates']:
            with Image.open(prepared / plate['file']) as image:
                image.resize((4096, 3072), Image.Resampling.LANCZOS).save(generated / (plate['id'] + '.png'))
        output = self.root / ('output-' + str(grid[0]))
        return atlas.assemble(prepared / 'manifest.json', generated, output), output, manifest

    def test_both_large_grids_preserve_exact_scroll_repeats(self):
        for grid in [(1, 1), (2, 1)]:
            report, output, manifest = self.run_assembly(grid)
            self.assertEqual(manifest['grid'], list(grid))
            row = report['images'][0]
            self.assertTrue(row['qa']['periodic_repeats_exact'])
            with Image.open(output / row['regions'][0]['path']) as a, Image.open(output / row['regions'][1]['path']) as b:
                self.assertEqual(a.size, (48, 32))
                self.assertEqual(a.tobytes(), b.tobytes())

    def test_nonperiodic_source_cannot_claim_scroll_continuity(self):
        self.pixels[9, 5, 0] ^= 32
        Image.fromarray(self.pixels).save(self.root / 'source.png')
        self.record['rgba_sha256'] = hashlib.sha256(self.pixels.tobytes()).hexdigest()
        with self.assertRaisesRegex(ValueError, 'not exactly periodic'):
            self.run_assembly((2, 1))


if __name__ == '__main__':
    unittest.main()
