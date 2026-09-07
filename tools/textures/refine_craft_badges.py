#!/usr/bin/env python3
"""Reconstruct original craft-number badges in authored UV orientation.

This emits localized overlays for a separately reviewed paint compositor.  It
does not select paint candidates or replace any live texture pack.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

S = 4
CANVAS = 640
FONT_DIR = Path('/System/Library/Fonts/Supplemental')
FONTS = {'plain': FONT_DIR / 'Arial Narrow.ttf',
         'bold': FONT_DIR / 'Arial Narrow Bold.ttf',
         'slab': FONT_DIR / 'Courier New Bold.ttf'}


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def coefficients(dst: np.ndarray, src: np.ndarray) -> np.ndarray:
    a, b = [], []
    for (x, y), (u, v) in zip(dst, src):
        a.extend([[x, y, 1, 0, 0, 0, -u*x, -u*y],
                  [0, 0, 0, x, y, 1, -v*x, -v*y]])
        b.extend([u, v])
    return np.linalg.solve(np.asarray(a), np.asarray(b))


def ordered_upright_quad(native: list) -> np.ndarray:
    q = np.asarray(native, dtype=float).copy()
    q[:, 1] = 32 - q[:, 1]
    angle = np.arctan2(q[:, 1] - q[:, 1].mean(), q[:, 0] - q[:, 0].mean())
    q = q[np.argsort(angle)]
    return np.roll(q, -np.argmin(q.sum(axis=1)), axis=0)


def text_mask(text: str, box: tuple, font_name='plain') -> Image.Image:
    font = ImageFont.truetype(str(FONTS[font_name]), 520)
    box0 = font.getbbox(text)
    glyph = Image.new('L', (box0[2]-box0[0], box0[3]-box0[1]))
    ImageDraw.Draw(glyph).text((-box0[0], -box0[1]), text, font=font, fill=255)
    l,t,r,b = [round(x*CANVAS) for x in box]
    glyph = glyph.resize((r-l,b-t), Image.Resampling.LANCZOS)
    mask = Image.new('L', (CANVAS,CANVAS))
    mask.paste(glyph,(l,t))
    return mask


def paint(im: Image.Image, color: tuple, mask: Image.Image) -> None:
    im.paste(Image.new('RGB', im.size, color), (0,0), mask)


def bezier(p0,p1,p2,p3,n=100):
    t=np.linspace(0,1,n)[:,None]
    return (1-t)**3*np.array(p0)+3*(1-t)**2*t*np.array(p1)+3*(1-t)*t*t*np.array(p2)+t**3*np.array(p3)


def rounded_stroke(draw, points, fill, width):
    # Pillow's float-coordinate wide polyline joins can leave radial cracks.
    # Overlapping round caps at sampled vertices make the contour continuous.
    draw.line(points, fill=fill, width=width)
    radius=width/2
    for x,y in points:
        draw.ellipse((x-radius,y-radius,x+radius,y+radius),fill=fill)


def canonical(rider: str, alternate: bool) -> tuple[Image.Image, Image.Image, str]:
    """Draw audited identities; black/white artwork stays separate from paint."""
    im=Image.new('RGB',(CANVAS,CANVAS),(0,0,0))
    alpha=Image.new('L',im.size)
    d=ImageDraw.Draw(im);m=ImageDraw.Draw(alpha)
    if rider=='R. Hayami':
        d.rounded_rectangle((0,0,639,639),radius=35,fill=(20,20,20))
        d.rounded_rectangle((29,29,610,610),radius=18,fill=(249,249,248))
        m.rounded_rectangle((0,0,639,639),radius=35,fill=255)
        paint(im,(18,18,18),text_mask('3',(.17,.10,.84,.91),'bold'))
        return im,alpha,'3'
    if rider=='M. Jeter':
        d.rectangle((0,0,639,639),fill=(39,6,104) if alternate else (17,8,78));m.rectangle((0,0,639,639),fill=255)
        number='7' if alternate else '1'
        mask=text_mask(number,(.22,.13,.79,.87),'slab' if not alternate else 'bold')
        paint(im,(225,226,218),mask.filter(ImageFilter.MaxFilter(71)))
        paint(im,(21,24,27),mask)
        return im,alpha,number
    if rider=='D. Mariner':
        # The original is a flat painted disk, not a raised metal medallion.
        d.ellipse((18,18,621,621),fill=(52,53,51));d.ellipse((36,36,603,603),fill=(17,18,19) if alternate else (230,231,229))
        m.ellipse((18,18,621,621),fill=255)
        number='95' if alternate else '22'
        paint(im,(232,233,230) if alternate else (37,38,37),text_mask(number,(.16,.15,.86,.85),'plain' if alternate else 'bold'))
        return im,alpha,number
    if rider=='A. Stewart':
        d.ellipse((65,46,600,608),fill=(92,6,38) if not alternate else (42,4,82))
        d.ellipse((89,68,577,585),fill=(247,247,240));m.ellipse((65,46,600,608),fill=255)
        if not alternate:
            # Original open 4: tall right stem and a diagonally closed crossbar.
            # Coordinates traced against the native yellow centerline.
            path=[(.57,.08),(.28,.65),(.77,.65)]
            pts=[tuple(v*CANVAS for v in p) for p in path]
            stem=[(.60*CANVAS,.08*CANVAS),(.60*CANVAS,.94*CANVAS)]
            d.polygon(pts,fill=(18,18,14))
            for color,width in [((18,18,14),105),((240,221,0),48)]:
                d.line(pts,fill=color,width=width,joint='curve');d.line(stem,fill=color,width=width)
            ImageDraw.Draw(alpha).line(pts,fill=255,width=93,joint='curve');ImageDraw.Draw(alpha).line(stem,fill=255,width=93)
            return im,alpha,'4'
        # Source 8 is a narrow continuous loop with a diagonal crossing.
        p=np.concatenate([bezier((.58,.08),(.20,.04),(.22,.33),(.52,.49)),
                          bezier((.52,.49),(.92,.72),(.63,1.02),(.37,.88)),
                          bezier((.37,.88),(.06,.65),(.64,.40),(.67,.24)),
                          bezier((.67,.24),(.70,.13),(.65,.08),(.58,.08))])
        points=[tuple(v*CANVAS) for v in p]
        d.ellipse((.23*CANVAS,.01*CANVAS,.75*CANVAS,.50*CANVAS),fill=(16,11,16))
        d.ellipse((.15*CANVAS,.45*CANVAS,.75*CANVAS,.99*CANVAS),fill=(16,11,16))
        rounded_stroke(d,points,fill=(16,11,16),width=106)
        rounded_stroke(d,points,fill=(222,0,78),width=62)
        rounded_stroke(ImageDraw.Draw(alpha),points,fill=255,width=106)
        return im,alpha,'8'
    raise ValueError(rider)


def build(selection: Path, output: Path) -> dict:
    if output.exists():
        raise FileExistsError(f'Refusing to overwrite {output}')
    output.mkdir(parents=True)
    records=json.loads(selection.read_text())['images']
    contact=Image.new('RGB',(1024,16*180),(42,42,42));cd=ImageDraw.Draw(contact)
    report=[]
    for i,r in enumerate(records):
        source=Image.open(r['source_png']).convert('RGBA')
        assert source.size==(64,32)
        current=Image.open(r['current_png']).convert('RGBA')
        assert current.size==(256,128)
        upright=current.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        mask=Image.new('L',(256,128))
        if r['rider']=='R. Hayami' and not r['alternate']:
            # The separately reviewed focused numeral is already correct.
            q=ordered_upright_quad(r['badge_quad_native'])
            hi=Image.new('L',(1024,512));ImageDraw.Draw(hi).polygon([tuple(x*16) for x in q],fill=255)
            mask=hi.resize(mask.size,Image.Resampling.LANCZOS).filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.GaussianBlur(.35))
            result=upright.copy();identity='2';method='Existing reviewed focused NB2 badge retained byte-exact'
        else:
            badge,alpha,identity=canonical(r['rider'],r['alternate'])
            if r['badge_quad_native']:
                q=ordered_upright_quad(r['badge_quad_native'])
            else:
                l,t,rr,b=r['badge_bounds_native']
                if r['rider']=='D. Mariner':
                    # Selection bounds include the blurred fringe.  Fit the
                    # circular disk to its 18 native white-pixel extent plus
                    # its half-texel border, not the rectangular audit region.
                    l,rr=(35.5,54.5) if l>30 else (9.5,28.5)
                    t,b=9,28
                q=np.array([[l,32-b],[rr,32-b],[rr,32-t],[l,32-t]],float)
            q*=S*4
            coeff=coefficients(q,np.array([[0,0],[CANVAS,0],[CANVAS,CANVAS],[0,CANVAS]],float))
            hi_size=(upright.width*4,upright.height*4)
            warped=badge.transform(hi_size,Image.Transform.PERSPECTIVE,coeff,Image.Resampling.BICUBIC).resize(upright.size,Image.Resampling.LANCZOS)
            mask=alpha.transform(hi_size,Image.Transform.PERSPECTIVE,coeff,Image.Resampling.BICUBIC).resize(upright.size,Image.Resampling.LANCZOS)
            result=Image.composite(warped.convert('RGBA'),upright,mask)
            method='Source-identified vector numeral and original badge footprint, projectively fitted in upright UV then restored native T orientation'
        result.putalpha(255)
        result=result.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        mask=mask.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        rgb=np.asarray(result);old=np.asarray(current);ma=np.asarray(mask)
        assert np.array_equal(rgb[ma==0],old[ma==0])
        png=output/(r['image_id']+'.png');maskpath=output/(r['image_id']+'-mask.png')
        result.save(png);mask.save(maskpath)
        report.append({'image_id':r['image_id'],'hash_aliases':r['aliases'],'output_png':str(png.resolve()),'mask_png':str(maskpath.resolve()),'output_size':[256,128],'source_png':r['source_png'],'source_png_sha256':sha(Path(r['source_png'])),'current_png_sha256':sha(Path(r['current_png'])),'rider':r['rider'],'alternate':r['alternate'],'upright_numeral':identity,'method':method,'native_badge_bounds':r['badge_bounds_native'],'native_badge_quad':r['badge_quad_native'],'png_sha256':sha(png),'mask_sha256':sha(maskpath),'outside_mask_byte_exact':True,'retained_current_byte_exact':bool(np.array_equal(rgb,old)),'mask_fraction':float(np.mean(ma>0))})
        cd.text((4,i*180+2),f'{i} {r["rider"]} {"alternate" if r["alternate"] else "normal"} number {identity}',fill='white')
        for j,(label,im) in enumerate([('Original',source),('Current',current),('Reconstructed badge',result)]):
            cd.text((j*340+4,i*180+19),label,fill='white')
            contact.paste(im.transpose(Image.Transpose.FLIP_TOP_BOTTOM).resize((256,128),Image.Resampling.NEAREST if j==0 else Image.Resampling.LANCZOS).convert('RGB'),(j*340+4,i*180+40))
    manifest={'status':'visual_review_required_not_installed','selection_sha256':sha(selection),'scope':'Localized primary badge overlays only; paint assembly owned separately. Authoritative original identities and native UV orientation preserved.','font_provenance':{k:{'path':str(v),'sha256':sha(v),'font_index':0} for k,v in FONTS.items()},'images':report}
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');contact.save(output/'comparison.png')
    return manifest


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--selection',type=Path,required=True);p.add_argument('--output',type=Path,required=True);args=p.parse_args()
    report=build(args.selection,args.output);print(json.dumps({'images':len(report['images']),'output':str(args.output)},indent=2))
