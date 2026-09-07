#!/usr/bin/env python3
"""Fit reviewed helmet portraits to the original opaque20x20 HUD canvases."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from PIL import Image

SCALE=8

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def ink_bounds(rgb,threshold=24):
    ys,xs=np.where(np.asarray(rgb).max(axis=2)>threshold)
    if not len(xs):raise ValueError('portrait foreground is empty')
    return [int(xs.min()),int(ys.min()),int(xs.max()+1),int(ys.max()+1)]

def fit_portrait(candidate,source,preserve_aspect=False,normalize_black=True):
    rgb=np.asarray(candidate.convert('RGB')).copy()
    # Authored portraits contain opaque black backgrounds. Remove only near-
    # black generated matte noise; do not paste blurred native helmet pixels.
    if normalize_black:rgb[rgb.max(2)<8]=0
    src=np.asarray(source.convert('RGB'))
    source_bounds=ink_bounds(src)
    generated_bounds=ink_bounds(rgb)
    target_bounds=[v*SCALE for v in source_bounds]
    part=Image.fromarray(rgb).crop(generated_bounds)
    target_size=(target_bounds[2]-target_bounds[0],target_bounds[3]-target_bounds[1])
    if preserve_aspect:
        scale=min(target_size[0]/part.width,target_size[1]/part.height)
        size=(max(1,round(part.width*scale)),max(1,round(part.height*scale)))
        placement=(target_bounds[2]-size[0],target_bounds[3]-size[1])
    else:size,placement=target_size,tuple(target_bounds[:2])
    part=part.resize(size,Image.Resampling.LANCZOS)
    result=Image.new('RGB',(20*SCALE,20*SCALE),(0,0,0));result.paste(part,placement)
    return result.convert('RGBA'),{'method':'selected sharp artwork fitted to original nonblack portrait extent on an opaque black canvas','source_foreground_bounds':source_bounds,'generated_foreground_bounds':generated_bounds,'output_foreground_registration_bounds':target_bounds,'actual_output_artwork_bounds':[*placement,placement[0]+size[0],placement[1]+size[1]],'preserve_aspect':preserve_aspect,'anchor':'bottom-right authored shoulder crop' if preserve_aspect else 'source bounding rectangle','foreground_threshold_u8':24,'near_black_matte_floor_u8':8 if normalize_black else None,'broad_source_detail_mask_used':False}

def build(inventory_path,manifest_path,generated_path,output):
    inventory=json.loads(inventory_path.read_text());manifest=json.loads(manifest_path.read_text())
    if sha(inventory_path)!=manifest['inventory_sha256']:raise ValueError('source inventory changed')
    if manifest.get('target_scale')!=SCALE:raise ValueError('portrait scale must remain8x')
    if output.exists():raise ValueError('refusing to overwrite a frozen portrait pack')
    cells={r['image_id']:r for r in manifest['cells']};plate=Image.open(generated_path).convert('RGB');prepared=[]
    for record in inventory['images']:
        source=Image.open(inventory_path.parent/record['png']).convert('RGBA')
        if source.size!=(20,20) or hashlib.sha256(source.tobytes()).hexdigest()!=record['rgba_sha256']:raise ValueError('trusted portrait source changed')
        if source.getextrema()[3]!=(255,255):raise ValueError('portrait source must be opaque')
        cell=cells[record['image_id']]
        if cell['source_rgba_sha256']!=record['rgba_sha256']:raise ValueError('candidate reference differs from source')
        rect=cell['crop_normalized']
        if len(rect)!=4 or not 0<=rect[0]<rect[2]<=1 or not 0<=rect[1]<rect[3]<=1:raise ValueError('invalid normalized portrait crop')
        crop=tuple(round(v*(plate.width if j%2==0 else plate.height)) for j,v in enumerate(rect));candidate=plate.crop(crop);result,qa=fit_portrait(candidate,source,manifest.get('preserve_artwork_aspect',False),manifest.get('normalize_near_black',True));prepared.append((record,result,{'sampled_generated_crop':list(crop),**qa}))
    output.mkdir(parents=True);(output/'textures').mkdir();images=[];mappings=[];seen=set()
    for record,image,qa in prepared:
        path='textures/'+record['image_id']+'.png';image.save(output/path)
        for key in record['hash_aliases']:
            if key in seen:raise ValueError('duplicate portrait alias')
            seen.add(key);mappings.append({'hashes':{'rt64':key},'path':path})
        images.append({'image_id':record['image_id'],'rider_name':record['rider_name'],'variant':record['variant'],'source_rom_offset':record['source_rom_offset'],'runtime_confirmed':record['runtime_confirmed'],'source_size':[20,20],'output_size':[160,160],'hash_aliases':record['hash_aliases'],'path':path,'png_sha256':sha(output/path),'opaque':True,'qa':qa})
    config={'autoPath':'rt64','configurationVersion':3,'hashVersion':5,'defaultOperation':'stream','defaultShift':'half'}
    (output/'rt64.json').write_text(json.dumps({'configuration':config,'textures':mappings},indent=2)+'\n')
    report={'schema_version':1,'status':'static_and_native_review_required','artwork_source_type':manifest.get('artwork_source_type','generated_candidate'),'artwork_source_path':str(generated_path),'generated_sha256':sha(generated_path),'artwork_source_sha256':sha(generated_path),'design_corrections_applied':False,'manifest_sha256':sha(manifest_path),'source_inventory_sha256':sha(inventory_path),'summary':{'source_images':len(images),'aliases':len(mappings),'scale':SCALE,'opaque':True,'preserve_png':True},'images':images}
    (output/'portrait-report.json').write_text(json.dumps(report,indent=2)+'\n');return report

def main():
    p=argparse.ArgumentParser();p.add_argument('--inventory',type=Path,required=True);p.add_argument('--manifest',type=Path,required=True);p.add_argument('--generated',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();print(json.dumps(build(a.inventory,a.manifest,a.generated,a.output)['summary']))
if __name__=='__main__':main()
