#!/usr/bin/env python3
"""No-AI atlas roundtrip, true sampling flags, fragments, and rejection metrics."""
from pathlib import Path
import hashlib
import importlib.util
import json
import tempfile
import unittest
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('atlas', ROOT / 'tools/textures/atlas.py')
atlas = importlib.util.module_from_spec(spec)
spec.loader.exec_module(atlas)


class AtlasTest(unittest.TestCase):
    def setUp(self):
        build = ROOT / 'build/texture-atlas-tests';build.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=build);self.directory = Path(self.temp.name)
        self.source = self.directory / 'input';self.source.mkdir()
        images=[];textures=[]
        for index,(width,height) in enumerate([(32,24),(32,32),(64,16)]):
            y,x=np.indices((height,width));rgba=np.zeros((height,width,4),dtype=np.uint8)
            rgba[...,0]=30+(x*5)%190;rgba[...,1]=40+(y*6)%180;rgba[...,2]=70+((x+y)*3)%160;rgba[...,3]=255
            if index==1:rgba[...,3]=np.rint(np.clip((13-np.sqrt((x-15.5)**2+(y-15.5)**2))/2,0,1)*255).astype(np.uint8)
            path=self.source/f'source-{index}.png';Image.fromarray(rgba).save(path)
            aliases=[f'{index+1:016x}']
            record={'image_id':f'source-{index}','png':path.name,'width':width,'height':height,
                    'rgba_sha256':hashlib.sha256(rgba.tobytes()).hexdigest(),'hash_aliases':aliases}
            if index==0:record['hash_aliases'].append('0000000000000010')
            if index==2:
                record['regions']=[{'id':'left','rect':[0,0,32,16],'hash_aliases':['0000000000000003']},
                                   {'id':'right','rect':[32,0,64,16],'hash_aliases':['0000000000000004','0000000000000005']}]
                aliases+=['0000000000000004','0000000000000005']
            images.append(record)
            for alias in record['hash_aliases']+aliases:
                textures.append({'hash':alias,'metadata':{'tile':{'cms':0 if index==0 else 2,'cmt':2,'masks':5,'maskt':0}}})
        self.inventory=self.source/'inventory.json';self.inventory.write_text(json.dumps({'images':images,'textures':textures,'errors':[]}))
        self.prepared=self.directory/'prepared'
        self.manifest=atlas.prepare(self.inventory,self.prepared,grid=(4,3))

    def tearDown(self):self.temp.cleanup()

    def generated(self,size=(4096,3072)):
        directory=self.directory/f'generated-{size[0]}-{size[1]}';directory.mkdir(exist_ok=True)
        for plate in self.manifest['plates']:
            with Image.open(self.prepared/plate['file']) as image:image.resize(size,Image.Resampling.LANCZOS).save(directory/f'{plate["id"]}.png')
        return directory

    def test_one_plate_coordinates_and_sampler_padding(self):
        self.assertEqual(len(self.manifest['plates']),1)
        for cell in self.manifest['cells']:
            l,t,r,b=cell['crop_rect'];self.assertEqual(cell['crop_normalized'],[l/1024,t/768,r/1024,b/768])
            cl,ct,cr,cb=cell['cell_rect'];pl,pt,pr,pb=cell['padding_rect']
            self.assertTrue(cl+8<=pl<pr<=cr-8 and ct+8<=pt<pb<=cb-8)
        cell=next(c for c in self.manifest['cells'] if c['image_id']=='source-0')
        self.assertEqual(cell['sampling']['s'],'wrap');self.assertEqual(cell['sampling']['t'],'edge')
        rgba=np.array([[[11,12,13,255],[21,22,23,255]]],dtype=np.uint8)
        padded=np.array(atlas.pad_rgba(Image.fromarray(rgba),2,'wrap','edge'))
        self.assertEqual(padded[2,:,0].tolist(),[11,21,11,21,11,21])
        self.assertEqual(atlas.axis_indices(3,2,'mirror').tolist(),[1,0,0,1,2,2,1])

    def check_roundtrip(self,size):
        report=atlas.assemble(self.prepared/'manifest.json',self.generated(size),self.directory/f'pack-{size[0]}')
        self.assertEqual(report['summary']['mapped_hashes'],6)
        self.assertEqual(report['summary']['replacement_pngs'],4)
        self.assertEqual(report['summary']['flagged_images'],0,report['images'])
        database=json.loads((self.directory/f'pack-{size[0]}'/'rt64.json').read_text())
        self.assertEqual(database['configuration']['hashVersion'],5)
        paths={t['hashes']['rt64']:t['path'] for t in database['textures']}
        self.assertEqual(paths['0000000000000001'],paths['0000000000000010'])
        self.assertEqual(paths['0000000000000004'],paths['0000000000000005'])
        for row in report['images']:
            self.assertTrue(row['qa']['alpha_matches_trusted_resample'])
            self.assertEqual(row['qa']['source_edge_rgb_max_error_u8'],0)
            self.assertLess(row['qa']['final']['low_frequency_rgb_rmse'],.025)
            self.assertEqual(row['qa']['review_status'],'visual_review_required')
            cell=next(c for c in self.manifest['cells'] if c['image_id']==row['image_id'])
            with Image.open(self.prepared/cell['source_rgba']) as source:
                alpha=source.getchannel('A').resize((source.width*4,source.height*4),Image.Resampling.LANCZOS)
            for region in row['regions']:
                with Image.open(self.directory/f'pack-{size[0]}'/region['path']) as image:
                    self.assertEqual(image.size,tuple(v*4 for v in region['original_size']))
                    self.assertTrue(np.array_equal(np.array(image.getchannel('A')),np.array(alpha.crop(tuple(v*4 for v in region['source_rect'])))))

    def test_exact_simulated_generated_upscale(self):self.check_roundtrip((4096,3072))
    def test_normalized_crop_handles_dimension_deviation(self):self.check_roundtrip((2053,1539))

    def test_structural_change_is_flagged_before_correction(self):
        generated=self.generated();cell=next(c for c in self.manifest['cells'] if c['image_id']=='source-0')
        path=generated/f'{cell["plate_id"]}.png'
        with Image.open(path) as image:
            image=image.convert('RGB');l,t,r,b=[v*4 for v in cell['crop_rect']]
            image.paste((107,109,146),(l,t,r,b));image.save(path)
        report=atlas.assemble(self.prepared/'manifest.json',generated,self.directory/'distorted')
        row=next(r for r in report['images'] if r['image_id']=='source-0')
        self.assertIn('generated_luminance_structure_lost',row['qa']['flags'])
        self.assertEqual(report['status'],'draft_visual_review_required')
        self.assertGreater(report['summary']['flagged_images'],0)

    def test_fake_detail_cannot_change_native_opaque_logo_boundaries(self):
        y,x=np.indices((24,32));rgba=np.zeros((24,32,4),dtype=np.uint8)
        rgba[...,0]=70+x*2;rgba[...,1]=80+y*2;rgba[...,2]=140;rgba[...,3]=255
        rgba[4:7,6:25,:3]=235;rgba[7:20,14:17,:3]=235
        source=Image.fromarray(rgba);trusted=atlas.resize_rgba(source,(128,96))
        pixels=np.asarray(trusted).copy();gy,gx=np.indices((96,128))
        # Mean-zero invented strokes can pass low-frequency checks while changing
        # a letter outline. The source-boundary mask must remove those strokes.
        perturb=np.where((gx+gy)%2,16,-16)
        pixels[...,:3]=np.clip(pixels[...,:3].astype(int)+perturb[...,None],0,255)
        output,qa=atlas.reconstruct(Image.fromarray(pixels).convert('RGB'),source)
        self.assertGreater(qa['applied_detail_strength'],0,qa)
        mask=atlas.detail_boundary_mask(rgba.astype(np.float32)/255,(128,96))
        self.assertGreater(np.count_nonzero(mask==1),0)
        self.assertTrue(np.array_equal(np.asarray(output)[mask==1],np.asarray(trusted)[mask==1]))
        self.assertEqual(qa['native_boundary_rgb_max_error_u8'],0)
        self.assertTrue(np.any(np.asarray(output)[mask==0]!=np.asarray(trusted)[mask==0]))

    def test_grayscale_effect_noise_and_flat_color_are_exact_source_resamples(self):
        y,x=np.indices((16,16));rgba=np.zeros((16,16,4),dtype=np.uint8)
        intensity=np.rint(np.clip(1-np.hypot(x-7.5,y-7.5)/11,0,1)*255).astype(np.uint8)
        rgba[...,:3]=intensity[...,None];rgba[...,3]=intensity
        source=Image.fromarray(rgba);noise=np.random.default_rng(21).integers(0,256,(64,64,3),dtype=np.uint8)
        output,qa=atlas.reconstruct(Image.fromarray(noise),source)
        self.assertIn('grayscale_soft_effect_or_ui',qa['fallback_reasons'])
        self.assertEqual(qa['candidate_status'],'source_preserving_4x')
        self.assertTrue(np.array_equal(np.asarray(output),np.asarray(atlas.resize_rgba(source,(64,64)))))
        source=Image.new('RGBA',(16,16),(90,130,160,255))
        output,qa=atlas.reconstruct(Image.fromarray(noise),source)
        self.assertIn('flat_source_color',qa['fallback_reasons'])
        self.assertTrue(np.array_equal(np.asarray(output),np.asarray(atlas.resize_rgba(source,(64,64)))))

    def test_severe_generated_color_change_uses_exact_source_fallback(self):
        with Image.open(self.source/'source-0.png') as opened:source=opened.convert('RGBA')
        size=(source.width*4,source.height*4)
        output,qa=atlas.reconstruct(Image.new('RGB',size,'white'),source)
        self.assertIn('generated_candidate_failed_structure_guard',qa['fallback_reasons'])
        self.assertEqual(qa['candidate_status'],'source_preserving_4x')
        self.assertTrue(np.array_equal(np.asarray(output),np.asarray(atlas.resize_rgba(source,size))))

    def test_visual_policy_can_reject_fake_lettering_inside_opaque_regions(self):
        policy=self.directory/'policy.json'
        policy.write_text(json.dumps({'schema_version':1,'images':{'source-0':{
            'detail_strength':0,'reason':'opaque logo lettering must preserve its source design'}}}))
        generated=self.generated();cell=next(c for c in self.manifest['cells'] if c['image_id']=='source-0')
        path=generated/f'{cell["plate_id"]}.png'
        with Image.open(path) as opened:
            image=opened.convert('RGB');l,t,r,b=[v*4 for v in cell['crop_rect']]
            image.paste((0,0,0),(l+30,t+30,l+40,t+100));image.save(path)
        report=atlas.assemble(self.prepared/'manifest.json',generated,self.directory/'policy-pack',policy_path=policy)
        row=next(r for r in report['images'] if r['image_id']=='source-0')
        self.assertEqual(row['qa']['candidate_status'],'source_preserving_4x')
        self.assertEqual(row['qa']['applied_detail_strength'],0)
        self.assertEqual(report['visual_review_policy_sha256'],hashlib.sha256(policy.read_bytes()).hexdigest())
        with Image.open(self.source/'source-0.png') as source:
            expected=atlas.resize_rgba(source.convert('RGBA'),(source.width*4,source.height*4))
        with Image.open(self.directory/'policy-pack'/row['regions'][0]['path']) as output:
            self.assertTrue(np.array_equal(np.asarray(output),np.asarray(expected)))

    def test_grid_and_source_hash_validation(self):
        result=atlas.prepare(self.inventory,self.directory/'dense',grid=(8,6),tile_size=96)
        self.assertEqual(result['grid'],[8,6]);self.assertEqual(len(result['plates']),1)
        with self.assertRaises(ValueError):atlas.prepare(self.inventory,self.directory/'bad',grid=(8,6),tile_size=110)
        with Image.open(self.source/'source-0.png') as image:
            image=image.convert('RGBA');image.putpixel((0,0),(1,2,3,4));image.save(self.source/'source-0.png')
        with self.assertRaisesRegex(ValueError,'source pixels differ'):
            atlas.prepare(self.inventory,self.directory/'changed')

    def test_duplicate_image_id_cannot_overwrite_trusted_source(self):
        inventory=json.loads(self.inventory.read_text())
        inventory['images'][1]['image_id']=inventory['images'][0]['image_id']
        self.inventory.write_text(json.dumps(inventory))
        with self.assertRaisesRegex(ValueError,'duplicate image_id'):
            atlas.prepare(self.inventory,self.directory/'duplicate-id')

    def test_joined_region_must_match_runtime_hash_dimensions(self):
        inventory=json.loads(self.inventory.read_text())
        inventory['textures'].append({'hash':'0000000000000003','width':64,'height':16})
        self.inventory.write_text(json.dumps(inventory))
        with self.assertRaisesRegex(ValueError,'differ from RT64 hash'):
            atlas.prepare(self.inventory,self.directory/'bad-region-dimensions')
        inventory['textures'][-1]['width']=32
        self.inventory.write_text(json.dumps(inventory))
        accepted=atlas.prepare(self.inventory,self.directory/'valid-region-dimensions')
        self.assertEqual(len(accepted['cells']),3)

    def test_large_joined_strip_is_flagged_when_dense_plate_loses_resolution(self):
        inventory=json.loads(self.inventory.read_text())
        with Image.open(self.source/'source-2.png') as image:
            image=image.resize((256,16),Image.Resampling.NEAREST);image.save(self.source/'source-2.png')
        record=inventory['images'][2]
        record.update(width=256,rgba_sha256=hashlib.sha256(image.tobytes()).hexdigest())
        record['regions'][0]['rect']=[0,0,128,16]
        record['regions'][1]['rect']=[128,0,256,16]
        self.inventory.write_text(json.dumps(inventory))
        self.manifest=atlas.prepare(self.inventory,self.directory/'dense-strip',grid=(8,6))
        self.prepared=self.directory/'dense-strip'
        report=atlas.assemble(self.prepared/'manifest.json',self.generated(),self.directory/'dense-strip-pack')
        row=next(r for r in report['images'] if r['image_id']=='source-2')
        self.assertIn('generated_crop_has_insufficient_resolution',row['qa']['flags'])
        self.assertEqual(row['qa']['review_status'],'visual_review_required')


if __name__=='__main__':unittest.main()
