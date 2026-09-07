#!/usr/bin/env python3
"""Assemble reviewed island detail with source material boundaries and UV edges."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from PIL import Image

SCALE = 8

def gaussian_filter(rgb, sigma, mode='nearest'):
    """Float-preserving Gaussian for coarse RGB fields, with clamped padding."""
    sy,sx,_=sigma
    py,px=int(np.ceil(4*sy)),int(np.ceil(4*sx))
    padded=np.pad(rgb,((py,py),(px,px),(0,0)),mode='edge')
    h,w=padded.shape[:2]
    fy=np.fft.fftfreq(h)[:,None];fx=np.fft.rfftfreq(w)[None,:]
    kernel=np.exp(-2*np.pi**2*(sy*sy*fy*fy+sx*sx*fx*fx))
    transformed=np.fft.rfftn(padded,axes=(0,1))
    result=np.fft.irfftn(transformed*kernel[:,:,None],s=(h,w),axes=(0,1))
    return result[py:py+rgb.shape[0],px:px+rgb.shape[1]]

def file_sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def smooth_threshold(value, low, high):
    t=np.clip((value-low)/(high-low),0,1)
    return t*t*(3-2*t)

def material_weights(rgb):
    """Soft green and submerged-cyan regions; the remainder is earth or sand."""
    r,g,b=np.moveaxis(rgb,-1,0)
    green=smooth_threshold(g-r,3,18)*smooth_threshold(g-b,4,18)
    water=smooth_threshold(np.minimum(g,b)-r,3,18)
    return green,water

def source_resize(source):
    return np.asarray(source.convert('RGB').resize((source.width*SCALE,source.height*SCALE),Image.Resampling.LANCZOS)).astype(float)

def fit_materials(candidate, source):
    trusted=source_resize(source)
    # Low-frequency palette registration leaves the generated leaf/grit detail
    # intact; no source-gradient protection mask is applied over the terrain.
    correction=gaussian_filter(trusted-candidate,sigma=(4*SCALE,4*SCALE,0),mode='nearest')
    fitted=np.clip(candidate+correction,0,255)
    source_regions=material_weights(trusted)
    generated_regions=material_weights(fitted)
    mismatch=np.maximum.reduce([np.abs(a-b) for a,b in zip(source_regions,generated_regions)])
    # Keep source green/earth/water boundaries where the generated sheet drifted.
    # Recolor only material disagreements, retaining a fine grayscale residual.
    detail=fitted-gaussian_filter(fitted,sigma=(1.5*SCALE,1.5*SCALE,0),mode='nearest')
    luma=detail @ np.array([.2126,.7152,.0722])
    recolored=np.clip(trusted+np.clip(luma,-40,40)[:,:,None],0,255)
    weight=smooth_threshold(mismatch,.18,.6)
    result=fitted*(1-weight[:,:,None])+recolored*weight[:,:,None]
    return result,{'coarse_palette_sigma_native_pixels':4,'source_material_recolor_fraction':float(np.mean(weight>0)), 'full_material_recolor_fraction':float(np.mean(weight>.99)), 'mean_absolute_rgb_palette_correction':float(np.mean(np.abs(correction)))}

def preserve_uv_edges(rgb,source):
    trusted=source_resize(source);h,w=rgb.shape[:2];yy,xx=np.indices((h,w));dist=np.minimum.reduce([xx,w-1-xx,yy,h-1-yy])
    weight=np.clip((SCALE-dist)/(SCALE-2),0,1)
    result=np.uint8(np.rint(np.clip(rgb*(1-weight[:,:,None])+trusted*weight[:,:,None],0,255)))
    expected=np.uint8(np.rint(trusted));edge=np.zeros((h,w),bool);edge[:2]=True;edge[-2:]=True;edge[:,:2]=True;edge[:,-2:]=True
    return result,{'edge_blend_fraction':float(np.mean(weight>0)),'exact_outer_two_pixels_rgb_max_error_u8':int(np.abs(result[edge].astype(int)-expected[edge].astype(int)).max())}

def build(inventory_path,manifest_path,generated_path,output):
    inventory=json.loads(inventory_path.read_text());manifest=json.loads(manifest_path.read_text())
    if file_sha(inventory_path)!=manifest['inventory_sha256']:raise ValueError('focused source inventory changed')
    if manifest.get('target_scale')!=SCALE:raise ValueError('terrain output must remain a uniform8x')
    if output.exists():raise ValueError('refusing to overwrite a frozen terrain pack')
    cells={r['image_id']:r for r in manifest['cells']};plate=Image.open(generated_path).convert('RGB');prepared=[]
    for record in inventory['images']:
        source=Image.open(inventory_path.parent/record['png']).convert('RGBA')
        if source.size!=(record['width'],record['height']) or hashlib.sha256(source.tobytes()).hexdigest()!=record['rgba_sha256']:raise ValueError('trusted source pixels changed')
        if source.getextrema()[3]!=(255,255):raise ValueError('island material must be opaque')
        cell=cells[record['image_id']]
        if cell['source_rgba_sha256']!=record['rgba_sha256']:raise ValueError('candidate source reference differs')
        candidate_path=Path(cell.get('generated_path',generated_path))
        candidate_plate=Image.open(candidate_path).convert('RGB') if candidate_path!=generated_path else plate
        if cell.get('source_manifest') and file_sha(cell['source_manifest'])!=cell['source_manifest_sha256']:raise ValueError('reviewed candidate manifest changed')
        rect=cell['crop_normalized']
        if len(rect)!=4 or not 0<=rect[0]<rect[2]<=1 or not 0<=rect[1]<rect[3]<=1:raise ValueError('invalid normalized terrain crop')
        crop=tuple(round(v*(candidate_plate.width if j%2==0 else candidate_plate.height)) for j,v in enumerate(rect));size=(source.width*SCALE,source.height*SCALE)
        raw=candidate_plate.crop(crop).resize(size,Image.Resampling.LANCZOS)
        rgb,qa=fit_materials(np.asarray(raw).astype(float),source);rgb,edge=preserve_uv_edges(rgb,source)
        prepared.append((record,Image.fromarray(rgb).convert('RGBA'),{'candidate_generated_path':str(candidate_path),'candidate_generated_sha256':file_sha(candidate_path),'candidate_source_manifest':cell.get('source_manifest',str(manifest_path)),'candidate_selection_note':cell.get('candidate_selection_note','Reviewed focused island terrain candidate'),'sampled_generated_crop':list(crop),'generated_crop_pixels_per_target_pixel':[(crop[2]-crop[0])/size[0],(crop[3]-crop[1])/size[1]],**qa,**edge}))
    output.mkdir(parents=True);(output/'textures').mkdir();images=[];mappings=[];seen=set()
    for record,image,qa in prepared:
        path='textures/'+record['image_id']+'.png';image.save(output/path)
        for key in record['hash_aliases']:
            if key in seen:raise ValueError('duplicate terrain alias')
            seen.add(key);mappings.append({'hashes':{'rt64':key},'path':path})
        images.append({'image_id':record['image_id'],'label':record['label'],'source_size':[record['width'],record['height']],'output_size':list(image.size),'hash_aliases':record['hash_aliases'],'path':path,'png_sha256':file_sha(output/path),'opaque':True,'qa':qa})
    config={'autoPath':'rt64','configurationVersion':3,'hashVersion':5,'defaultOperation':'stream','defaultShift':'half'}
    (output/'rt64.json').write_text(json.dumps({'configuration':config,'textures':mappings},indent=2)+'\n')
    report={'schema_version':1,'status':'static_and_native_review_required','generated_sha256':file_sha(generated_path),'manifest_sha256':file_sha(manifest_path),'source_inventory_sha256':file_sha(inventory_path),'summary':{'source_images':len(images),'aliases':len(mappings),'scale':SCALE,'opaque':True},'images':images}
    (output/'terrain-report.json').write_text(json.dumps(report,indent=2)+'\n');return report

def main():
    p=argparse.ArgumentParser();p.add_argument('--inventory',type=Path,required=True);p.add_argument('--manifest',type=Path,required=True);p.add_argument('--generated',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();print(json.dumps(build(a.inventory,a.manifest,a.generated,a.output)['summary']))
if __name__=='__main__':main()
