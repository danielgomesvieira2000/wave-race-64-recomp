#!/usr/bin/env python3
"""Build the byte-identified HUD rank arrow as a crisp, source-sized 8x icon.

The native HUD always draws this upward texture; rank-change flashing is an
environment tint applied outside the image. No speculative down-arrow keys are
created. The SVG and separately supersampled fill/outline retain that tinting.
"""
from pathlib import Path
import argparse
import hashlib
import json
import struct
import subprocess

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
SOURCE_ID = "4182e2d5ba6211222b8114d62a73f5be74d9dc76c54d70cd4aae1a700543570e"
SOURCE_HASH = "0e3e5120bb31f0f9"
ROM_OFFSET = 0x129CF0
SCALE = 8
OUTLINE = [(4, .30), (7.65, 6.90), (5.50, 6.90), (5.50, 8.95),
           (2.50, 8.95), (2.50, 6.90), (.35, 6.90)]
FILL = [(4, 1.15), (6.85, 6.20), (5, 6.20), (5, 8.30),
        (3, 8.30), (3, 6.20), (1.15, 6.20)]


def rounded_geometry(points, radius):
    """Leave the triangle and stem straight; round only tiny corner segments."""
    points=np.asarray(points,dtype=float)
    commands=[]; samples=[]
    for index, point in enumerate(points):
        toward_previous=points[index-1]-point
        toward_next=points[(index+1)%len(points)]-point
        before=point+toward_previous/np.linalg.norm(toward_previous)*radius
        after=point+toward_next/np.linalg.norm(toward_next)*radius
        commands.append(f'{"M" if index==0 else "L"} {before[0]},{before[1]} Q {point[0]},{point[1]} {after[0]},{after[1]}')
        for t in np.linspace(0,1,17):
            samples.append(((1-t)**2*before+2*t*(1-t)*point+t*t*after).tolist())
    return samples, ' '.join(commands)+' Z'


def mask(points, supersample=8, symmetric=True):
    factor = SCALE * supersample
    high = Image.new("L", (8 * factor, 10 * factor))
    ImageDraw.Draw(high).polygon([(round(x * factor), round(y * factor)) for x, y in points], fill=255)
    # Area integration gives a one-output-pixel contour without a blurry halo.
    result=np.asarray(high.resize((64, 80), Image.Resampling.BOX), dtype=np.uint8)
    if symmetric:
        # Integrate both mirrored raster edge conventions around the exact
        # vector center, removing the rasterizer's inclusive-edge bias.
        result=((result.astype(np.uint16)+result[:,::-1]+1)//2).astype(np.uint8)
    return result


def build(output, previous_pack):
    output = output.resolve()
    if not output.is_relative_to(ROOT) or not output.relative_to(ROOT).parts[0].startswith('build'):
        raise ValueError("output must be an ignored build directory")
    source = ROOT / "build/textures-nb2/decoded-paused/images" / f"{SOURCE_ID}.png"
    original = Image.open(source).convert('RGBA')
    encoded = bytes(((int(r)//17)<<4) | (int(a)//17) for r, g, b, a in np.asarray(original).reshape(-1, 4))
    rom = (ROOT / 'reference/wr64-decomp/baserom.us.rev1.z64').read_bytes()
    assert original.size == (8, 10) and rom[ROM_OFFSET:ROM_OFFSET+80] == encoded
    assert hashlib.sha256(struct.pack('>II', 8, 10) + original.tobytes()).hexdigest() == SOURCE_ID

    inventory_paths = subprocess.check_output(['rg', '-l', SOURCE_ID,
        str(ROOT / 'build/textures-nb2'), '-g', '*inventory*.json'], text=True).splitlines()
    aliases, samplers, contexts = set(), {}, []
    for path in map(Path, inventory_paths):
        if path.is_relative_to(output):
            continue
        value = json.loads(path.read_text())
        for record in value.get('images', []):
            if record.get('image_id') == SOURCE_ID:
                assert (record['width'], record['height']) == original.size
                assert record['rgba_sha256'] == hashlib.sha256(original.tobytes()).hexdigest()
                aliases.update(record['hash_aliases'])
                contexts.append(str(path))
        for record in value.get('textures', []):
            if record.get('image_id') == SOURCE_ID:
                samplers[record['hash']] = record['metadata']
    assert SOURCE_HASH in aliases
    for metadata in samplers.values():
        assert metadata['width'] == 8 and metadata['height'] == 10
        assert metadata['tile']['cms'] == metadata['tile']['cmt'] == 2

    (output / 'pack/textures').mkdir(parents=True, exist_ok=True)
    (output / 'sources').mkdir(exist_ok=True)
    source_copy = output / 'sources' / f'{SOURCE_ID}.png'
    original.save(source_copy)
    source_rgba=np.asarray(original,dtype=np.float32)/255
    outline, outline_path=rounded_geometry(OUTLINE,.11)
    fill, fill_path=rounded_geometry(FILL,.09)
    alpha = mask(outline)
    white = np.minimum(mask(fill),alpha)
    rgb = np.divide(white.astype(float) * 255, alpha, out=np.zeros(alpha.shape), where=alpha > 0)
    result = Image.fromarray(np.dstack([np.rint(rgb).astype(np.uint8)] * 3 + [alpha]))
    pixels = np.asarray(result)
    assert result.size == (64, 80)
    assert not any(np.any(edge) for edge in [alpha[:2], alpha[-2:], alpha[:, :2], alpha[:, -2:]])
    assert np.all(pixels[alpha == 0, :3] == 0)
    source_alpha_area=float(source_rgba[...,3].sum())
    source_white_area=float((source_rgba[...,0]*source_rgba[...,3]).sum())
    target_alpha_area=float(alpha.sum()/255/SCALE**2)
    target_white_area=float(white.sum()/255/SCALE**2)
    assert np.array_equal(alpha,alpha[:,::-1]) and np.array_equal(white,white[:,::-1])
    solid=np.argwhere(source_rgba[...,0]*source_rgba[...,3] >= .5)
    solid_bounds=[int(solid[:,1].min()),int(solid[:,0].min()),int(solid[:,1].max()+1),int(solid[:,0].max()+1)]
    white_bounds=Image.fromarray(white).getbbox()
    assert white_bounds[0]>=solid_bounds[0]*SCALE and white_bounds[1]>=solid_bounds[1]*SCALE
    assert white_bounds[2]<=solid_bounds[2]*SCALE and white_bounds[3]<=solid_bounds[3]*SCALE
    target = output / 'pack/textures' / f'{SOURCE_ID}.png'
    result.save(target)
    polygons = f'<path d="{outline_path}" fill="black"/><path d="{fill_path}" fill="white"/>'
    (output / 'rank-arrow.svg').write_text('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="80" viewBox="0 0 8 10">' + polygons + '</svg>\n')
    previous_database = json.loads((previous_pack / 'rt64.json').read_text())
    previous_record = next(r for r in previous_database['textures'] if r['hashes']['rt64'] == SOURCE_HASH)
    previous_path = previous_pack / previous_record['path']
    previous = Image.open(previous_path).convert('RGBA')
    database = {'configuration': previous_database['configuration'], 'textures': [
        {'path': f'textures/{SOURCE_ID}.png', 'hashes': {'rt64': key}} for key in sorted(aliases)]}
    (output / 'pack/rt64.json').write_text(json.dumps(database, indent=2) + '\n')
    record = dict(image_id=SOURCE_ID, png=str(source_copy), width=8, height=10,
        rgba_sha256=hashlib.sha256(original.tobytes()).hexdigest(), hash_aliases=sorted(aliases),
        label='HUD rank threshold upward arrow', format='IA8', source_rom_offset=hex(ROM_OFFSET),
        source_segmented_address='0x01033c60', output_png=str(target), output_size=[64,80], scale=8,
        output_png_sha256=hashlib.sha256(target.read_bytes()).hexdigest(), source_inventories=sorted(set(contexts)))
    inventory = {'schema_version': 1, 'images': [record], 'sampler_metadata_by_alias': samplers}
    (output / 'source-inventory.json').write_text(json.dumps(inventory, indent=2) + '\n')
    report = dict(source_image_count=1, alias_count=len(aliases), aliases=sorted(aliases),
        family_proof='B97B0.s VRAM 801F5AB4 compares rank against threshold, changes environment tint, then both branches use D_1033C60 at 801F5B28. Texture rectangle uses positive 0x04000400 gradients at 801F5C68. No alternate arrow source or UV inversion in this block.',
        method='Symmetric triangular head and rectangular stem, fitted inside original solid white coverage bounds; 0.11/0.09 native-texel corner rounding and 8x8 coverage integration per output pixel.',
        original_sampler_preserved=True, png_kept_without_mips=True,
        transparent_border_output_pixels=2, transparent_rgb_zero=True,
        output_alpha_bounds=list(result.getchannel('A').getbbox()),
        source_alpha_area=source_alpha_area,
        output_alpha_area_in_source_pixels=target_alpha_area,
        source_white_area=source_white_area,
        output_white_area_in_source_pixels=target_white_area,
        alpha_coverage_relative_error=target_alpha_area/source_alpha_area-1,
        white_coverage_relative_error=target_white_area/source_white_area-1,
        output_partial_alpha_pixels=int(np.count_nonzero((alpha>0)&(alpha<255))),
        source_solid_white_bounds=solid_bounds, output_white_bounds=list(white_bounds),
        mirror_symmetric=True, placement_uses_alpha_fringe=False,
        previous_png=str(previous_path), previous_size=list(previous.size),
        pack=str(output/'pack'), inventory=str(output/'source-inventory.json'),
        review_status='source geometry reconstructed; native review pending')
    (output / 'validation.json').write_text(json.dumps(report, indent=2) + '\n')
    contact = Image.new('RGB', (1050, 520), (28, 38, 49))
    draw = ImageDraw.Draw(contact)
    font = ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial.ttf', 20)
    for column, (title, image, filter_) in enumerate([
        ('Original 8x10 (nearest)', original, Image.Resampling.NEAREST),
        ('Current 32x40 (bilinear)', previous, Image.Resampling.BILINEAR),
        ('Vector 64x80 (nearest)', result, Image.Resampling.NEAREST)]):
        x = column*350
        draw.text((x+14,14), title, fill='white', font=font)
        for y, size in [(58,(240,300)),(394,(64,80))]:
            background=Image.new('RGBA', size, (16,74,214,255))
            background.alpha_composite(image.resize(size, filter_))
            contact.paste(background.convert('RGB'), (x+40,y))
    draw.text((14,490), 'Same 8x10 canvas and RT64 sampler; white fill keeps native tint behavior.', fill='white', font=font)
    contact.save(output/'contact.png')
    print(json.dumps(report))


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'build/textures-nb2/branding-review/rank-arrow')
    parser.add_argument('--previous-pack', type=Path, default=ROOT/'build/textures-nb2/branding-review/candidate-pack-v5')
    args=parser.parse_args()
    build(args.output, args.previous_pack)
