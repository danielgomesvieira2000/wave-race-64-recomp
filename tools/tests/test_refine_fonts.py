"""Font geometry, matte, provenance and joined-fragment regression checks."""
from pathlib import Path
import hashlib
import importlib.util
import json
import tempfile
import unittest

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('refine_fonts', ROOT / 'tools/textures/refine_fonts.py')
fonts = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fonts)


class FontTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def source(self):
        image = Image.new('RGBA', (16, 12), (255, 255, 255, 0))
        draw = ImageDraw.Draw(image)
        draw.rectangle((3, 2, 11, 9), fill='white')
        draw.rectangle((5, 4, 9, 7), fill=(255, 255, 255, 0))
        return image

    def test_contour_preserves_counter_and_white_color(self):
        image, report = fonts.refine_font(self.source())
        self.assertEqual(image.size, (64, 48))
        pixels = np.array(image)
        self.assertEqual(pixels[24, 28, 3], 0)
        self.assertEqual(pixels[12, 16, 3], 255)
        self.assertTrue(np.all(pixels[..., :3][pixels[..., 3] > 0] == 255))
        self.assertEqual(report['family'], 'plain-white')

    def test_generated_matte_removes_background_and_counter(self):
        plate = Image.new('RGB', (192, 128), (64, 64, 64))
        draw = ImageDraw.Draw(plate)
        draw.rectangle((40, 24, 151, 103), fill=(8, 8, 8))
        draw.rectangle((48, 32, 143, 95), fill=(245, 180, 0))
        draw.rectangle((72, 48, 119, 79), fill=(64, 64, 64))
        cell = {'image_id': 'test', 'cell_rect': [0, 0, 96, 64]}
        image, report = fonts.extract_nb2_font(plate, cell, self.source(), (96, 64))
        pixels = np.array(image)
        self.assertEqual(image.size, (64, 48))
        self.assertEqual(pixels[0, 0, 3], 0)
        self.assertEqual(pixels[24, 28, 3], 0)
        self.assertEqual(report['matte_rgb'], [64, 64, 64])
        self.assertEqual(report['sampled_cell_rect'], [0, 0, 192, 128])
        self.assertGreater(np.count_nonzero(pixels[..., 3] == 255), 200)

    def catalog(self):
        image = self.source()
        image.save(self.directory / 'source.png')
        row = {'image_id': 'audited-source', 'png': 'source.png', 'width': 16, 'height': 12,
               'rgba_sha256': hashlib.sha256(np.array(image).tobytes()).hexdigest(),
               'hash_aliases': ['0000000000000001'],
               'regions': [{'rect': [0, 0, 16, 6], 'hash_aliases': ['0000000000000001', '0000000000000002']},
                           {'rect': [0, 6, 16, 12], 'hash_aliases': ['0000000000000003']}]}
        path = self.directory / 'catalog.json'
        path.write_text(json.dumps({'schema_version': 1, 'images': [row]}))
        return path

    def test_joined_font_split_is_exact_and_aliases_share_png(self):
        path = self.catalog()
        pack = self.directory / 'pack'
        report = fonts.build_pack(path, pack)
        self.assertEqual(report['summary']['mapped_hashes'], 3)
        database = json.loads((pack / 'rt64.json').read_text())
        rows = database['textures']
        self.assertEqual(rows[0]['path'], rows[1]['path'])
        joined = Image.new('RGBA', (64, 48))
        joined.paste(Image.open(pack / rows[0]['path']), (0, 0))
        joined.paste(Image.open(pack / rows[2]['path']), (0, 24))
        reference, _ = fonts.refine_font(self.source())
        np.testing.assert_array_equal(np.array(joined), np.array(reference))
        self.assertEqual(report['status'], 'draft_native_review_required')

    def test_changed_source_is_rejected(self):
        path = self.catalog()
        Image.new('RGBA', (16, 12), 'red').save(self.directory / 'source.png')
        with self.assertRaisesRegex(ValueError, 'source content/dimensions changed'):
            fonts.build_pack(path, self.directory / 'pack')

    def test_overlapping_strips_preserve_overread_without_fitting_it(self):
        visible = self.source()
        source = Image.new('RGBA', (16, 13))
        source.paste(visible, (0, 0))
        for x in range(16):
            source.putpixel((x, 12), (255, x * 16, 80, 255))
        source.save(self.directory / 'source.png')
        record = {'image_id': 'overread', 'png': 'source.png', 'width': 16, 'height': 13,
                  'rgba_sha256': hashlib.sha256(source.tobytes()).hexdigest(),
                  'visible_art_height': 12, 'preserve_original_rows': [12], 'hash_aliases': [],
                  'regions': [{'rect': [0, 0, 16, 7], 'hash_aliases': ['0000000000000001']},
                              {'rect': [0, 6, 16, 13], 'hash_aliases': ['0000000000000002']}]}
        path = self.directory / 'catalog.json'
        path.write_text(json.dumps({'schema_version': 1, 'images': [record]}))
        pack = self.directory / 'pack'
        report = fonts.build_pack(path, pack)
        regions = report['images'][0]['regions']
        top = np.array(Image.open(pack / regions[0]['path']))
        bottom = np.array(Image.open(pack / regions[1]['path']))
        # Shared rows of overlapping transfers must be identical, not padded or
        # independently filtered, and next-asset samples must remain exact.
        np.testing.assert_array_equal(top[24:28], bottom[:4])
        expected, _ = fonts.refine_font(visible)
        np.testing.assert_array_equal(bottom[:24], np.array(expected)[24:48])
        overread = source.crop((0, 12, 16, 13)).resize((64, 4), Image.Resampling.NEAREST)
        np.testing.assert_array_equal(bottom[24:], np.array(overread))
        self.assertEqual(report['images'][0]['qa']['preserved_original_rows'], [12])
        # A model reference contains only the visible artwork, not the following
        # asset's overread row. Validate against that cropped reference digest.
        generated = self.directory / 'nb2/generated'
        generated.mkdir(parents=True)
        plate = Image.new('RGB', (192, 128), (64, 64, 64))
        ImageDraw.Draw(plate).rectangle((40, 24, 151, 103), fill=(30, 220, 70))
        plate.save(generated / 'plate-0000.png')
        cell = {'image_id': 'overread', 'plate_id': 'plate-0000', 'cell_rect': [0, 0, 192, 128],
                'source_rgba_sha256': hashlib.sha256(visible.tobytes()).hexdigest()}
        manifest = generated.parent / 'manifest.json'
        manifest.write_text(json.dumps({'plates': [{'id': 'plate-0000', 'size': [192, 128]}], 'cells': [cell]}))
        candidate_pack = self.directory / 'candidate-pack'
        candidate = fonts.build_pack(path, candidate_pack, [manifest])
        candidate_bottom = np.array(Image.open(candidate_pack / candidate['images'][0]['regions'][1]['path']))
        candidate_visible, _ = fonts.extract_nb2_font(plate, cell, visible, (192, 128))
        np.testing.assert_array_equal(candidate_bottom[:24], np.array(candidate_visible)[24:48])
        np.testing.assert_array_equal(candidate_bottom[24:], np.array(overread))

    def test_unaccounted_overread_rows_are_rejected(self):
        path = self.catalog()
        catalog = json.loads(path.read_text())
        catalog['images'][0]['visible_art_height'] = 11
        path.write_text(json.dumps(catalog))
        with self.assertRaisesRegex(ValueError, 'explicitly preserved'):
            fonts.build_pack(path, self.directory / 'pack')

    def test_out_of_bounds_generated_crop_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'invalid generated font cell'):
            fonts.extract_nb2_font(Image.new('RGB', (100, 100)),
                                   {'cell_rect': [-1, 0, 96, 64]}, self.source(), (96, 64))

    @unittest.skipUnless(Path('/System/Library/Fonts/Helvetica.ttc').exists(), 'local source-matched font unavailable')
    def test_vector_word_keeps_source_extent_and_white_strokes(self):
        image, report = fonts.vector_font(self.source(), 'O', Path('/System/Library/Fonts/Helvetica.ttc'), 1)
        self.assertEqual(image.size, (64, 48))
        pixels = np.array(image)
        self.assertEqual(pixels[24, 28, 3], 0)
        self.assertTrue(np.all(pixels[..., :3][pixels[..., 3] > 0] == 255))
        self.assertEqual(report['text'], 'O')
        self.assertEqual(report['font_family'], ['Helvetica', 'Bold'])
        self.assertEqual(len(report['font_file_sha256']), 64)
        occupied = np.argwhere(pixels[..., 3] > 0)
        self.assertGreaterEqual(int(occupied[:, 1].min()), 10)
        self.assertLessEqual(int(occupied[:, 1].max()), 49)

    def test_vector_text_requires_explicit_font(self):
        path = self.catalog()
        catalog = json.loads(path.read_text())
        catalog['images'][0]['vector_text'] = 'O'
        path.write_text(json.dumps(catalog))
        with self.assertRaisesRegex(ValueError, 'requires --vector-font'):
            fonts.build_pack(path, self.directory / 'pack')

    @unittest.skipUnless(Path('/System/Library/Fonts/Helvetica.ttc').exists(), 'local source-matched font unavailable')
    def test_vector_outline_retains_black_edge_and_open_counter(self):
        source = Image.new('RGBA', (20, 20))
        draw = ImageDraw.Draw(source)
        draw.ellipse((2, 2, 17, 17), fill='black')
        draw.ellipse((3, 3, 16, 16), fill='white')
        draw.ellipse((6, 6, 13, 13), fill=(0, 0, 0, 0))
        image, report = fonts.vector_font(source, 'O', Path('/System/Library/Fonts/Helvetica.ttc'), 1, True)
        pixels = np.array(image)
        self.assertEqual(pixels[40, 40, 3], 0)
        self.assertGreater(np.count_nonzero((pixels[..., :3].max(axis=2) < 10) & (pixels[..., 3] > 220)), 50)
        self.assertGreater(np.count_nonzero((pixels[..., :3].min(axis=2) > 245) & (pixels[..., 3] > 220)), 100)
        self.assertTrue(report['outlined'])

    @unittest.skipUnless(Path('/System/Library/Fonts/Helvetica.ttc').exists(), 'local source-matched font unavailable')
    def test_vector_text_region_does_not_change_adjacent_icon(self):
        path = self.catalog()
        catalog = json.loads(path.read_text())
        catalog['images'][0]['vector_parts'] = [{'rect': [8, 0, 16, 12], 'text': 'I'}]
        path.write_text(json.dumps(catalog))
        pack = self.directory / 'pack'
        report = fonts.build_pack(path, pack, vector_font_path=Path('/System/Library/Fonts/Helvetica.ttc'), vector_font_index=1)
        top = Image.open(pack / report['images'][0]['regions'][0]['path'])
        reference = self.source().resize((64, 48), Image.Resampling.LANCZOS)
        np.testing.assert_array_equal(np.array(top)[:, :32], np.array(reference)[:24, :32])
        self.assertEqual(report['summary']['vector_fonts'], 1)

    def test_reviewed_crop_excludes_copied_style_reference(self):
        path = self.catalog()
        catalog = json.loads(path.read_text())
        record = catalog['images'][0]
        record['nb2_sample_rect'] = [40, 30, 160, 90]
        path.write_text(json.dumps(catalog))
        generated = self.directory / 'nb2/generated'
        generated.mkdir(parents=True)
        plate = Image.new('RGB', (200, 100), (64, 64, 64))
        draw = ImageDraw.Draw(plate)
        draw.rectangle((10, 5, 50, 20), fill=(255, 0, 0))
        draw.rectangle((60, 40, 140, 80), fill=(245, 180, 0))
        plate.save(generated / 'plate-0000.png')
        manifest = {'plates': [{'id': 'plate-0000', 'size': [200, 100]}],
                    'cells': [{'image_id': record['image_id'], 'source_rgba_sha256': record['rgba_sha256'],
                               'plate_id': 'plate-0000', 'cell_rect': [0, 0, 200, 100]}]}
        manifest_path = generated.parent / 'manifest.json'
        manifest_path.write_text(json.dumps(manifest))
        pack = self.directory / 'pack'
        report = fonts.build_pack(path, pack, [manifest_path])
        self.assertEqual(report['images'][0]['qa']['sampled_cell_rect'], [40, 30, 160, 90])
        pixels = np.array(Image.open(pack / report['images'][0]['regions'][0]['path']))
        self.assertTrue(np.all(pixels[..., 1][pixels[..., 3] > 200] > 100))

    def test_placement_ignores_faint_antialiasing_fringe(self):
        field = np.zeros((40, 64), dtype=np.float32)
        field[:, :] = .3
        field[4:36, 5:59] = 1
        self.assertEqual(fonts.safe_ink_bounds(field), [5, 4, 59, 36])
        with self.assertRaisesRegex(ValueError, 'no placement bounds'):
            fonts.safe_ink_bounds(np.zeros((40, 64), dtype=np.float32))

    @unittest.skipUnless(Path('/System/Library/Fonts/Helvetica.ttc').exists(), 'local source-matched font unavailable')
    def test_pause_cap_does_not_reach_clamped_canvas_edges(self):
        pixels = np.full((10, 32, 4), 255, dtype=np.uint8)
        pixels[0, :, 3] = 100
        source = Image.fromarray(pixels)
        rendered, report = fonts.vector_font(source, 'TEST', Path('/System/Library/Fonts/Helvetica.ttc'), 1)
        alpha = np.array(rendered.getchannel('A'))
        self.assertEqual(rendered.size, (128, 40))
        self.assertEqual(int(alpha[:2].max()), 0)
        self.assertEqual(int(alpha[-2:].max()), 0)
        self.assertEqual(int(alpha[:, :2].max()), 0)
        self.assertEqual(int(alpha[:, -2:].max()), 0)
        # This models the renderer's clamp path when filter taps exceed the last
        # texel: no terminal stroke may be stretched below the word rectangle.
        clamped = np.pad(alpha, ((0, 12), (0, 0)), mode='edge')
        self.assertEqual(int(clamped[40:].max()), 0)
        self.assertEqual(report['canvas_guard_output_pixels'], 2)

    def test_guard_moves_small_punctuation_instead_of_trimming_it(self):
        source = Image.new('RGBA', (16, 16))
        ImageDraw.Draw(source).rectangle((14, 14, 15, 15), fill='white')
        result, report = fonts.guard_font_canvas(source)
        self.assertEqual(result.size, source.size)
        self.assertEqual(result.getchannel('A').getbbox(), (12, 12, 14, 14))
        self.assertEqual(np.array(result.getchannel('A')).sum(), 4 * 255)
        self.assertTrue(report['adjusted'])


if __name__ == '__main__':
    unittest.main()
