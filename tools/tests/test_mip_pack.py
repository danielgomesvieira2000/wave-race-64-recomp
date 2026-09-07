"""Validate DDS uploads with RT64's actual ddspp parser and alpha edge fixtures."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('build_mip_pack', ROOT / 'tools/textures/build_mip_pack.py')
mip = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mip)


class MipPackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        (ROOT / 'build/texture-qa').mkdir(parents=True, exist_ok=True)
        cls.temporary = tempfile.TemporaryDirectory(prefix='mip-test-', dir=ROOT / 'build/texture-qa')
        cls.directory = Path(cls.temporary.name)
        cls.validator = cls.directory / 'ddspp-probe'
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-isystem', str(ROOT / 'lib/RT64/src/contrib/ddspp'),
                        str(ROOT / 'tools/tests/ddspp_probe.cpp'), '-o', str(cls.validator)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_headers_mip_offsets_and_exact_top_level(self):
        for width, height in [(1, 1), (1, 9), (9, 1), (7, 5), (256, 128)]:
            with self.subTest(width=width, height=height):
                rng = np.random.default_rng(width * 1000 + height)
                rgba = rng.integers(0, 256, (height, width, 4), dtype=np.uint8)
                image = Image.fromarray(rgba)
                payload, levels = mip.dds_bytes(image)
                path = self.directory / f'{width}-{height}.dds'
                path.write_bytes(payload)
                parsed = json.loads(subprocess.check_output([str(self.validator), str(path)], text=True))
                self.assertEqual((parsed['width'], parsed['height']), (width, height))
                self.assertEqual(parsed['mip_count'], len(levels))
                self.assertEqual(parsed['header_bytes'], mip.DDS_HEADER_BYTES)
                self.assertEqual(parsed['mips'], [{key: value for key, value in level.items() if key != 'level'} for level in levels])
                offset = parsed['header_bytes']
                self.assertEqual(payload[offset:offset + width * height * 4], rgba.tobytes())

    def test_transparent_hidden_color_does_not_bleed(self):
        source = Image.fromarray(np.array([[[255, 0, 0, 255], [0, 0, 255, 0]]], dtype=np.uint8))
        levels = mip.mip_chain(source)
        self.assertEqual(levels[-1], (1, 1, bytes([255, 0, 0, 128])))
        hidden = Image.fromarray(np.array([[[255, 0, 0, 0], [0, 255, 255, 0]]], dtype=np.uint8))
        self.assertEqual(mip.mip_chain(hidden)[-1][2], bytes(4))

    def test_odd_edge_contributes_to_last_mip(self):
        rgba = np.zeros((1, 5, 4), dtype=np.uint8)
        rgba[..., 3] = 255
        rgba[0, -1, :3] = 255
        self.assertEqual(mip.mip_chain(Image.fromarray(rgba))[-1][2], bytes([51, 51, 51, 255]))

    def test_policy_preserves_ui_hash_and_mapping_properties(self):
        source = self.directory / 'source-pack'
        source.mkdir()
        (source / 'textures').mkdir()
        original = source / 'textures/shared.png'
        Image.new('RGBA', (7, 5), (20, 100, 80, 170)).save(original)
        database = {'configuration': {'hashVersion': 5, 'defaultShift': 'half'}, 'textures': [
            {'hashes': {'rt64': 'world'}, 'path': 'textures/shared.png', 'operation': 'preload'},
            {'hashes': {'rt64': 'ui'}, 'path': 'textures/shared.png', 'shift': 'none'}]}
        (source / 'rt64.json').write_text(json.dumps(database))
        output = self.directory / 'dds-pack'
        result = mip.build_pack(source, output, {'schema_version': 1, 'preserve_png_hashes': ['ui']}, self.validator)
        converted = json.loads((output / 'rt64.json').read_text())
        self.assertEqual(converted['configuration'], database['configuration'])
        self.assertEqual(converted['textures'][0]['operation'], 'preload')
        self.assertEqual(converted['textures'][0]['path'], 'textures/shared.dds')
        self.assertEqual(converted['textures'][1], database['textures'][1])
        self.assertEqual((output / 'textures/shared.png').read_bytes(), original.read_bytes())
        self.assertEqual(result['converted_images'], 1)
        self.assertEqual(result['retained_png_images'], 1)
        self.assertEqual(len(result['actual_ddspp_validation']), 1)


if __name__ == '__main__':
    unittest.main()
