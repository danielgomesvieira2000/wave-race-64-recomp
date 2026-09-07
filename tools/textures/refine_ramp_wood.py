#!/usr/bin/env python3
"""Fit reviewed NB2 ramp grain while retaining native seams and painted markings."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw

SCALE = 8

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def resized_rgb(source, size):
    return np.asarray(source.convert('RGB').resize(size, Image.Resampling.LANCZOS)).astype(np.float64)

def fit_palette(rgb, source, vertical=False, painted=False):
    """One color offset per authored plank; never restore blurred local grain."""
    target = resized_rgb(source, (rgb.shape[1], rgb.shape[0]))
    h, w = rgb.shape[:2]
    yy, xx = np.indices((h, w))
    offsets = []
    for side in range(2):
        region = (xx < w // 2) if vertical else (yy < h // 2)
        if side: region = ~region
        region &= (xx > 3*SCALE) & (xx < w-3*SCALE) & (yy > SCALE) & (yy < h-SCALE)
        if painted:
            region &= ~((xx > 20*SCALE) & (xx < 44*SCALE) & (yy > 6*SCALE) & (yy < 25*SCALE))
            region &= (xx > 6*SCALE) & (xx < w-6*SCALE)
        offset = np.median(target[region], axis=0) - np.median(rgb[region], axis=0)
        offsets.append(offset.tolist())
        apply = (xx < w//2) if vertical else (yy < h//2)
        if side: apply = ~apply
        rgb[apply] += offset
    return np.clip(rgb, 0, 255), offsets

def remove_generated_paint(rgb):
    """Erase only generated central red paint before source-bound reconstruction."""
    h,w=rgb.shape[:2]
    mask=(rgb[:,:,0]>rgb[:,:,1]*1.45)&(rgb[:,:,0]>rgb[:,:,2]*1.4)
    mask[:6*SCALE]=False; mask[25*SCALE:]=False
    mask[:,:18*SCALE]=False; mask[:,46*SCALE:]=False
    for y in range(h):
        xs=np.flatnonzero(mask[y])
        if not len(xs): continue
        left=max(6*SCALE,int(xs[0])-5);right=min(w-6*SCALE,int(xs[-1])+6)
        l=np.median(rgb[max(0,y-2):min(h,y+3),left-3:left],axis=(0,1))
        r=np.median(rgb[max(0,y-2):min(h,y+3),right:right+3],axis=(0,1))
        t=np.linspace(0,1,right-left)[:,None]
        rgb[y,left:right]=l*(1-t)+r*t
    return rgb

def restore_paint(rgb, source):
    original=np.asarray(source.convert('RGB')).astype(np.float64)
    h,w=rgb.shape[:2];ss=4
    # Native red-mask spans are x32 at y8 through x24..39 at y22.
    # The continuous triangle follows the same original pixel-cell footprint.
    mask=Image.new('L',(w*ss,h*ss));draw=ImageDraw.Draw(mask)
    points=[(32.5*SCALE,8*SCALE),(24*SCALE,23*SCALE),(40*SCALE,23*SCALE)]
    draw.polygon([(round(x*ss),round(y*ss)) for x,y in points],fill=255)
    alpha=np.asarray(mask.resize((w,h),Image.Resampling.LANCZOS)).astype(float)/255
    row_colors=[]
    for y in range(32):
        candidates=original[y,23:41]
        red=(candidates[:,0]>candidates[:,1]*1.45)&(candidates[:,0]>candidates[:,2]*1.4)
        row_colors.append(np.median(candidates[red],axis=0) if red.any() else [190,30,25])
    gradient=np.asarray(Image.fromarray(np.uint8(np.clip(np.asarray(row_colors)[:,None],0,255))).resize((w,h),Image.Resampling.BILINEAR)).astype(float)
    rgb=rgb*(1-alpha[:,:,None])+gradient*alpha[:,:,None]
    # Original edge marks occupy five native columns on each side and change
    # red to white at the authored y16 plank boundary. Sample their original
    # shading independently of the generated wood, with crisp paint boundaries.
    target=resized_rgb(source,(w,h))
    for sl in [slice(0,5*SCALE),slice(w-5*SCALE,w)]:
        rgb[:,sl]=target[:,sl]
    return rgb, {'triangle_native_vertices':[[32.5,8],[24,23],[40,23]],'edge_mark_columns':[0,5,59,64],'red_white_transition_native_y':16,'method':'source-color triangle with supersampled native footprint; exact original painted edge strips'}

def preserve_seams(rgb, source, vertical=False):
    h,w=rgb.shape[:2];original=resized_rgb(source,(w,h));yy,xx=np.indices((h,w))
    dist=np.minimum.reduce([xx,w-1-xx,yy,h-1-yy]).astype(float)
    # Only the outermost two output samples are exact; taper within one native
    # texel. This retains original repeat transitions without erasing grain.
    weight=np.clip((SCALE-dist)/(SCALE-2),0,1)
    # Preserve the authored central join within a narrow half-native-texel band.
    axis=xx if vertical else yy;center=w//2 if vertical else h//2
    weight=np.maximum(weight,np.clip((SCALE/2-np.abs(axis-(center-.5)))/(SCALE/2),0,1))
    rgb=rgb*(1-weight[:,:,None])+original*weight[:,:,None]
    return np.uint8(np.rint(np.clip(rgb,0,255))),weight

def build(source_inventory, manifest_path, generated, output):
    inventory=json.loads(source_inventory.read_text());manifest=json.loads(manifest_path.read_text())
    if digest(source_inventory)!=manifest['inventory_sha256']: raise ValueError('source inventory changed')
    if output.exists(): raise ValueError('refusing to overwrite a frozen ramp pack')
    plate=Image.open(generated).convert('RGB');cells={r['image_id']:r for r in manifest['cells']}
    results=[];mappings=[];output.mkdir(parents=True);(output/'textures').mkdir()
    for i,record in enumerate(inventory['images']):
        source=Image.open(source_inventory.parent/record['png']).convert('RGBA')
        if source.size!=(64,32) or hashlib.sha256(source.tobytes()).hexdigest()!=record['rgba_sha256']:raise ValueError('trusted source changed')
        if source.getextrema()[3]!=(255,255):raise ValueError('ramp source must be opaque')
        cell=cells[record['image_id']];rect=cell['crop_normalized'];crop=tuple(round(v*(plate.width if j%2==0 else plate.height)) for j,v in enumerate(rect))
        candidate=plate.crop(crop).resize((512,256),Image.Resampling.LANCZOS);rgb=np.asarray(candidate).astype(float)
        # Remove the model's new perimeter bevel by extending adjacent grain;
        # no internal grain warping or whole-image source-detail mask is used.
        rgb[:5]=rgb[5];rgb[-5:]=rgb[-6];rgb[:,:5]=rgb[:,5:6];rgb[:,-5:]=rgb[:,-6:-5]
        painted='arrow' in record['label'];vertical='end grain' in record['label']
        if painted:rgb=remove_generated_paint(rgb)
        rgb,offsets=fit_palette(rgb,source,vertical,painted)
        paint=None
        if painted:rgb,paint=restore_paint(rgb,source)
        rgb,weights=preserve_seams(rgb,source,vertical)
        final=Image.fromarray(rgb).convert('RGBA');name='textures/'+record['image_id']+'.png';final.save(output/name)
        trusted=resized_rgb(source,(512,256)).round().astype(np.uint8)
        edge_error=max(int(np.abs(rgb[:2].astype(int)-trusted[:2]).max()),int(np.abs(rgb[-2:].astype(int)-trusted[-2:]).max()),int(np.abs(rgb[:,:2].astype(int)-trusted[:,:2]).max()),int(np.abs(rgb[:,-2:].astype(int)-trusted[:,-2:]).max()))
        for alias in record['hash_aliases']:mappings.append({'hashes':{'rt64':alias},'path':name})
        results.append({'image_id':record['image_id'],'label':record['label'],'source_size':[64,32],'output_size':[512,256],'scale':SCALE,'hash_aliases':record['hash_aliases'],'path':name,'png_sha256':digest(output/name),'sampled_generated_crop':list(crop),'per_plank_rgb_offset':offsets,'paint_reconstruction':paint,'narrow_seam_blend_fraction':float(np.mean(weights>0)),'exact_border_rgb_max_error_u8':edge_error,'opaque_pixels':512*256})
    config={'autoPath':'rt64','configurationVersion':3,'hashVersion':5,'defaultOperation':'stream','defaultShift':'half'}
    (output/'rt64.json').write_text(json.dumps({'configuration':config,'textures':mappings},indent=2)+'\n')
    report={'schema_version':1,'status':'static_review_required_native_validation_owned_by_root','generated_sha256':digest(generated),'manifest_sha256':digest(manifest_path),'source_inventory_sha256':digest(source_inventory),'source_provenance':inventory.get('source_provenance'),'images':results,'summary':{'sources':len(results),'aliases':len(mappings),'scale':SCALE,'opaque':True,'dds_mips_required':10}}
    (output/'ramp-report.json').write_text(json.dumps(report,indent=2)+'\n');return report

def main():
    p=argparse.ArgumentParser();p.add_argument('--inventory',type=Path,required=True);p.add_argument('--manifest',type=Path,required=True);p.add_argument('--generated',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();print(json.dumps(build(a.inventory,a.manifest,a.generated,a.output)['summary']))
if __name__=='__main__':main()
