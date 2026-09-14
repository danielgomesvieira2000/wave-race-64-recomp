"""Turn a Dolphin-named texture pack into an RT64 texture-pack mod, by matching pictures.

    python tools/match_texture_pack.py <pack dir> <rt64 dump dir> <mod dir>
        [--id my_pack] [--name "My Pack"] [--author Name] [--version 1.0.0]
        [--sheets <dir>]

then

    python tools/pack_mod.py <mod dir> --install

A pack made for Wave Race 64 on the Wii Virtual Console under Dolphin names its images
tex1_<W>x<H>_<hash>[_<tlut hash>]_<GX format>.png, and those hashes are of the textures
the Virtual Console's emulator had already converted. Nothing in the N64 data produces
them. What does carry over is the picture: <W>x<H> is the texture's own size, and the
image is an upscale of it. So every pack image is shrunk to <W>x<H> and compared with
every texture of that size in an RT64 dump (see tools/rt64_texture_dump.py), and a clear
match gives it the RT64 hash the port's texture replacement looks up.

Per pack image, among the dump textures of its size:

  colour error   mean |difference| of premultiplied RGBA, 0-255
  correlation    of luminance, which survives the Virtual Console's palette shifts
  alpha diff     mean |difference| of alpha

The five with the lowest colour error are re-ranked: one within 1.5x the lowest error
with a correlation 0.05 higher wins. Then:

  sure      clearly ahead of the next *different* picture (1.25x or 4 more error), and
            correlation >= 0.90, alpha diff < 16, and colour error < 10 -- or < 25 with
            correlation >= 0.93. A near-featureless texture (luminance and alpha standard
            deviation both < 8) needs colour error < 3.
  review    colour error < 35 and correlation >= 0.85.
  none      anything else, or no dump texture of that size.

Accepted: every 'sure' match, and 'review' matches with colour error < 20 and correlation
>= 0.905 -- above 20 the review matches measured here were the wrong colour variant of
the right shape (a rider's helmet, a sponsor logo). Each dump picture (the RT64 hashes of
one identical decoded image) then keeps only its lowest-error pack image, so similar
frames cannot all claim one texture; ties prefer images outside a folder named
'Upscaled'. File names starting 'Alternate - ' are skipped.

A second, shape pass then fills dump pictures the colour pass left without an image
(shape_matches): sizes padded to multiples of 4 -- the Virtual Console pads every
texture, so an N64 80x3 strip is named 80x4 with its content top-left -- and a mutual
best match on luminance/alpha correlation, which accepts redrawn lettering the colour
error rejects. It never overrides a colour-pass choice.

Writes into <mod dir>: mod.json, rt64.json (hash version 5, the layout the port's other
packs use) and textures/ with only the images used; match_report.json beside it with
every pack image's scores and verdict. --sheets writes contact sheets -- each pair is the
pack image then the dump texture, on magenta for transparency -- of a random sample of
accepted 'sure' matches, of every accepted 'review' match and of every shape-pass match,
which is what to look at before trusting a new pack or a new dump.

Measured on Nicolas's pack (2,372 images, 2,242 distinct pictures) against a
1,542-texture dump: 940 textures replaced -- 756 by colour (88 of 88 sampled correct)
and 184 by shape, reviewed by eye, where the mutual-best rule contradicted none of the
colour pass's matches. See docs/TEXTURE-PACK-MATCHING.md.

Needs numpy and Pillow.
"""
import argparse
import collections
import json
import os
import random
import re
import shutil
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rt64_texture_dump  # noqa: E402

NAME = re.compile(r"^tex1_(\d+)x(\d+)_([0-9a-f]{16})(?:_([0-9a-f]{16}|\$))?_(\d+)(?:_mip\d+)?\.png$", re.I)


def premultiplied(rgba):
    rgba = rgba.astype(np.float32)
    return np.concatenate([rgba[..., :3] * (rgba[..., 3:4] / 255.0), rgba[..., 3:4]], -1)


def luminance(pm):
    return pm[..., 0] * 0.299 + pm[..., 1] * 0.587 + pm[..., 2] * 0.114


def correlation(a, b):
    a = a.ravel() - a.mean()
    b = b.ravel() - b.mean()
    d = np.sqrt((a * a).sum() * (b * b).sum())
    if d <= 1e-6:
        return 1.0 if np.allclose(a, b) else 0.0
    return float((a * b).sum() / d)


def shrink(path, w, h):
    """The pack image at w x h: premultiplied, area filter."""
    arr = np.asarray(Image.open(path).convert("RGBA"))
    pm = np.clip(premultiplied(arr), 0, 255).astype(np.uint8)
    return np.asarray(Image.fromarray(pm, "RGBA").resize((w, h), Image.BOX)).astype(np.float32)


def score_pack(pack_dir, dump):
    by_size = collections.defaultdict(list)
    for n, img in dump.items():
        by_size[(img.shape[1], img.shape[0])].append(n)
    stacks = {size: (names, np.stack([premultiplied(dump[n]) for n in names]))
              for size, names in by_size.items()}

    report = []
    for dirpath, _, files in sorted(os.walk(pack_dir)):
        for f in sorted(files):
            m = NAME.match(f)
            if not m:
                continue
            rel = os.path.relpath(os.path.join(dirpath, f), pack_dir).replace(os.sep, "/")
            w, h = int(m[1]), int(m[2])
            entry = {"pack": rel, "w": w, "h": h, "verdict": "none",
                     "upscaled_folder": rel.split("/")[0].lower() == "upscaled"}
            report.append(entry)
            if (w, h) not in stacks:
                entry["reason"] = "no dump texture of this size"
                continue
            small = shrink(os.path.join(dirpath, f), w, h)
            names, stack = stacks[(w, h)]
            err = np.abs(stack - small[None]).mean(axis=(1, 2, 3))
            scored = []
            for i in np.argsort(err)[:5]:
                scored.append((int(i), float(err[i]), correlation(luminance(stack[i]), luminance(small)),
                               float(np.abs(stack[i][..., 3] - small[..., 3]).mean())))
            best = scored[0]
            for cand in scored[1:]:
                if cand[1] <= best[1] * 1.5 and cand[2] > best[2] + 0.05:
                    best = cand
            bi, be, bc, bad = best
            best_img = dump[names[bi]]
            same = [names[i] for i in range(len(names)) if np.array_equal(dump[names[i]], best_img)]
            different = [e for (i, e, c, a) in scored if not np.array_equal(dump[names[i]], best_img)]
            runner = min(different) if different else None
            clear = runner is None or runner > be * 1.25 or runner - be > 4
            pm = stack[bi]
            featureless = bool(luminance(pm).std() < 8 and pm[..., 3].std() < 8)
            if featureless:
                sure = be < 3 and clear
            else:
                sure = clear and bc >= 0.90 and bad < 16 and (be < 10 or (be < 25 and bc >= 0.93))
            if sure:
                verdict = "sure"
            elif be < 35 and bc >= 0.85:
                verdict = "review"
            else:
                verdict = "none"
            entry.update({"best": names[bi], "error": be, "correlation": bc, "alpha_diff": bad,
                          "runner_error": runner, "featureless": featureless,
                          "same_picture": same, "verdict": verdict})
    return report


def pad4(v):
    return (v + 3) // 4 * 4


def _batch_correlation(stack, small):
    a = stack.reshape(len(stack), -1)
    a = a - a.mean(1, keepdims=True)
    b = small.ravel() - small.mean()
    den = np.sqrt((a * a).sum(1) * (b * b).sum())
    return np.where(den > 1e-6, (a @ b) / np.maximum(den, 1e-6), 0.0)


def shape_matches(pack_dir, dump):
    """The second pass: mutual-best shape matches, with padded sizes.

    The Virtual Console pads every texture to multiples of 4 (every Dolphin name is), so an
    N64 80x3 texture is a pack image named 80x4 with its content in the top-left 80x3.
    And a redrawn image -- crisp lettering over the original's blocky glyphs -- can be the
    right picture at a colour error of 40-60. So for every pack image whose size is a dump
    texture's size or that size padded, at the texture's native size:

      shape = mean(luminance correlation, alpha correlation) -- luminance alone when
              either side has no alpha structure (std <= 5)

    A pack image and a dump picture are paired when each is the other's best shape match,
    shape >= 0.70, the pack image's margin over its next picture >= 0.02, and either
    that margin >= 0.10 or colour error < 25. The last condition is what removes a
    same-shaped wrong texture (red bars against grass): a small margin with a high error.
    """
    group_of, key_to_group = {}, {}
    for n, img in dump.items():
        group_of[n] = key_to_group.setdefault((img.shape, img.tobytes()), len(key_to_group))
    members = collections.defaultdict(list)
    for n, g in group_of.items():
        members[g].append(n)

    by_native = collections.defaultdict(list)
    for n, img in dump.items():
        by_native[(img.shape[1], img.shape[0])].append(n)
    natives_for_pack_size = collections.defaultdict(set)
    for (w, h) in by_native:
        natives_for_pack_size[(pad4(w), pad4(h))].add((w, h))
    stacks = {size: (names, np.stack([premultiplied(dump[n]) for n in names])) for size, names in by_native.items()}

    best = {}   # (pack rel, group) -> (shape, error)
    for dirpath, _, files in sorted(os.walk(pack_dir)):
        for f in sorted(files):
            m = NAME.match(f)
            if not m:
                continue
            pw, ph = int(m[1]), int(m[2])
            natives = natives_for_pack_size.get((pw, ph))
            if not natives:
                continue
            rel = os.path.relpath(os.path.join(dirpath, f), pack_dir).replace(os.sep, "/")
            arr = np.asarray(Image.open(os.path.join(dirpath, f)).convert("RGBA"))
            H, W = arr.shape[:2]
            for (w, h) in sorted(natives):
                cw, ch = max(1, round(W * w / pw)), max(1, round(H * h / ph))
                pm = np.clip(premultiplied(arr[:ch, :cw]), 0, 255).astype(np.uint8)
                small = np.asarray(Image.fromarray(pm, "RGBA").resize((w, h), Image.BOX)).astype(np.float32)
                names, stack = stacks[(w, h)]
                err = np.abs(stack - small[None]).mean(axis=(1, 2, 3))
                lum = _batch_correlation(luminance(stack), luminance(small))
                alpha = _batch_correlation(stack[..., 3], small[..., 3])
                has_alpha = (stack[..., 3].reshape(len(stack), -1).std(1) > 5) & (small[..., 3].std() > 5)
                shape = np.where(has_alpha, (lum + alpha) / 2, lum)
                for k, n in enumerate(names):
                    key = (rel, group_of[n])
                    if key not in best or shape[k] > best[key][0]:
                        best[key] = (float(shape[k]), float(err[k]))

    by_pack = collections.defaultdict(list)
    by_group = collections.defaultdict(list)
    for (rel, g), (shape, err) in best.items():
        by_pack[rel].append((shape, -err, g))
        by_group[g].append((shape, -err, rel))
    for v in list(by_pack.values()) + list(by_group.values()):
        v.sort(reverse=True)

    matches = []
    for rel, ranked in sorted(by_pack.items()):
        shape, negerr, g = ranked[0]
        margin = shape - (ranked[1][0] if len(ranked) > 1 else -1.0)
        if shape < 0.70 or margin < 0.02 or not (margin >= 0.10 or -negerr < 25):
            continue
        if by_group[g][0][2] != rel:
            continue
        matches.append({"pack": rel, "same_picture": sorted(members[g]), "shape": shape,
                        "margin": margin, "error": -negerr})
    return matches


def select(report, shape=()):
    accepted = [e for e in report if e["verdict"] == "sure" or
                (e["verdict"] == "review" and e["error"] < 20 and e["correlation"] >= 0.905)]
    groups = collections.defaultdict(list)
    for e in accepted:
        groups[tuple(sorted(e["same_picture"]))].append(e)
    selected = []
    for hashes, group in sorted(groups.items()):
        group.sort(key=lambda e: (round(e["error"], 1), e["upscaled_folder"], e["pack"]))
        winner = group[0]
        winner["selected"] = True
        for loser in group[1:]:
            loser["lost_to"] = winner["pack"]
        selected.append({"entry": winner, "hashes": [h.split(".")[0] for h in hashes]})
    # The shape pass fills pictures the colour pass left without an image; it never
    # overrides one the colour pass chose.
    claimed = {h for s in selected for h in s["hashes"]}
    by_pack = {e["pack"]: e for e in report}
    for m in shape:
        hashes = [h.split(".")[0] for h in m["same_picture"]]
        if any(h in claimed for h in hashes):
            continue
        entry = by_pack[m["pack"]]
        entry.update({"shape_match": {k: m[k] for k in ("shape", "margin", "error")}, "selected": True})
        selected.append({"entry": dict(entry, verdict="shape", error=m["error"], correlation=m["shape"],
                                       best=m["same_picture"][0]), "hashes": hashes})
        claimed.update(hashes)
    return selected


def write_mod(pack_dir, mod_dir, selected, args):
    if os.path.exists(mod_dir):
        sys.exit(f"{mod_dir} already exists; remove it or pick another directory")
    os.makedirs(os.path.join(mod_dir, "textures"))
    textures, used = [], set()
    for s in selected:
        base = os.path.basename(s["entry"]["pack"])
        name, k = base, 1
        while name in used:
            stem, ext = os.path.splitext(base)
            name, k = f"{stem}_{k}{ext}", k + 1
        used.add(name)
        shutil.copyfile(os.path.join(pack_dir, s["entry"]["pack"]), os.path.join(mod_dir, "textures", name))
        for h in s["hashes"]:
            textures.append({"hashes": {"rt64": h}, "path": f"textures/{name}"})
    database = {
        "configuration": {"autoPath": "rt64", "configurationVersion": 3, "hashVersion": 5,
                          "defaultOperation": "stream", "defaultShift": "half"},
        "textures": sorted(textures, key=lambda t: t["hashes"]["rt64"]),
    }
    with open(os.path.join(mod_dir, "rt64.json"), "w") as f:
        json.dump(database, f, indent=4)
    mod = {
        "game_id": "wr64",
        "id": args.id,
        "display_name": args.name,
        "short_description": f"{len(textures)} textures matched from a Dolphin texture pack. Local build.",
        "description": ("A texture pack made for the Virtual Console release under Dolphin, matched to "
                        "this port's textures by picture with tools/match_texture_pack.py. The images "
                        "belong to their author; check before redistributing."),
        "version": args.version,
        "authors": [args.author],
        "minimum_recomp_version": "0.6.0",
        "enabled_by_default": True,
    }
    with open(os.path.join(mod_dir, "mod.json"), "w") as f:
        json.dump(mod, f, indent=4)
    return len(textures)


def tile(src, box):
    im = Image.fromarray(src, "RGBA") if isinstance(src, np.ndarray) else src.convert("RGBA")
    bg = Image.new("RGBA", im.size, (255, 0, 255, 255))
    bg.alpha_composite(im)
    k = box / max(im.size)
    return bg.resize((max(1, int(im.width * k)), max(1, int(im.height * k))), Image.NEAREST).convert("RGB")


def contact_sheet(path, pack_dir, dump, entries):
    cols, cw, ch = 8, 140, 92
    sheet = Image.new("RGB", (cols * cw, max(1, (len(entries) + cols - 1) // cols) * ch), (30, 30, 30))
    draw = ImageDraw.Draw(sheet)
    for k, e in enumerate(entries):
        left, top = (k % cols) * cw, (k // cols) * ch
        sheet.paste(tile(Image.open(os.path.join(pack_dir, e["pack"])), 64), (left + 2, top + 2))
        sheet.paste(tile(dump[e["best"]], 64), (left + 70, top + 2))
        draw.text((left + 2, top + 70), f'{k}: e{e["error"]:.0f} c{e["correlation"]:.2f}', fill=(220, 220, 220))
    sheet.save(path)
    with open(path + ".json", "w") as f:
        json.dump([e["pack"] for e in entries], f, indent=1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("pack", help="the Dolphin-named pack's directory (searched recursively)")
    ap.add_argument("dump", help="an RT64 texture dump directory")
    ap.add_argument("mod", help="the mod directory to create")
    ap.add_argument("--id", default="dolphin_texture_pack")
    ap.add_argument("--name", default="Texture Pack")
    ap.add_argument("--author", default="Unknown")
    ap.add_argument("--version", default="1.0.0")
    ap.add_argument("--sheets", help="write review contact sheets here")
    args = ap.parse_args()

    dump = {}
    for n in rt64_texture_dump.names(args.dump):
        img = rt64_texture_dump.decode(args.dump, n)
        if img is not None and img.size:
            dump[n] = img
    print(f"dump: {len(dump)} textures decoded")

    report = score_pack(args.pack, dump)
    selected = select(report, shape_matches(args.pack, dump))
    count = write_mod(args.pack, args.mod, selected, args)
    report_path = os.path.join(os.path.dirname(os.path.abspath(args.mod)), "match_report.json")
    with open(report_path, "w") as f:
        json.dump(report, f, indent=1)

    verdicts = collections.Counter(e["verdict"] for e in report)
    no_size = sum(1 for e in report if e.get("reason"))
    from_review = sum(1 for s in selected if s["entry"]["verdict"] == "review")
    from_shape = sum(1 for s in selected if s["entry"]["verdict"] == "shape")
    print(f"pack images: {len(report)} -- colour pass: sure {verdicts['sure']}, review {verdicts['review']}, "
          f"none {verdicts['none']} ({no_size} with no dump texture of their exact size)")
    print(f"replaced: {count} RT64 textures from {len(selected)} images "
          f"({from_review} from review, {from_shape} from the shape pass); {len(dump) - count} dump textures left as they are")
    print(f"mod: {args.mod}\nreport: {report_path}")

    if args.sheets:
        os.makedirs(args.sheets, exist_ok=True)
        chosen = [s["entry"] for s in selected]
        random.seed(1)
        sample = random.sample([e for e in chosen if e["verdict"] == "sure"],
                               min(48, sum(1 for e in chosen if e["verdict"] == "sure")))
        contact_sheet(os.path.join(args.sheets, "sure_sample.png"), args.pack, dump, sample)
        contact_sheet(os.path.join(args.sheets, "review_accepted.png"), args.pack, dump,
                      [e for e in chosen if e["verdict"] == "review"])
        contact_sheet(os.path.join(args.sheets, "shape_accepted.png"), args.pack, dump,
                      [e for e in chosen if e["verdict"] == "shape"])
        print(f"sheets: {args.sheets}")


if __name__ == "__main__":
    main()
