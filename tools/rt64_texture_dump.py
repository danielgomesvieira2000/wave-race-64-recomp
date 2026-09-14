"""Decode an RT64 texture dump into RGBA images.

    python tools/rt64_texture_dump.py <dump dir> <out dir>     # every texture as a PNG

RT64's developer menu (F1 -> Textures -> Start dumping textures) writes each texture
the game loads as a set of files named by its RT64 hash, hash version 5:

    <hash>.v5.tile.json            the draw: format, size, tile parameters, TLUT kind
    <hash>.v5.tmem                 RT64's TMEM image, 4096 bytes
    <hash>.v5.rice.json            the load operation (Tile or Block) Rice hashing needs
    <hash>.v5.rice.rdram           the raw RDRAM the texture was loaded from
    <hash>.v5.rice.palette.rdram   the palette's RDRAM, for colour-indexed textures

There is no image in it. This decodes the Rice files, which are plain N64 memory, rather
than the TMEM image, which carries RT64-internal layout (32-bit textures split across two
halves, swapped odd rows, 8-byte palette entries).

Two things decide whether the result is right:

- **Size and row stride** come from RT64's own Rice hasher,
  lib/RT64/src/tools/texture_hasher/texture_hasher.cpp, addRiceHash(), transcribed line
  for line in geometry(). The width and height are then capped at the tile.json's.
- **Byte order.** RT64 writes the RDRAM as little-endian 32-bit words -- its Rice CRC reads
  it that way -- so every 4 bytes are reversed before the texels are read big-endian.
  Without that, text comes out scrambled in 4-byte chunks and most textures are noise.

Import it for the arrays: decode(dump_dir, name) -> (h, w, 4) uint8, names(dump_dir).
Needs numpy and Pillow.
"""
import json
import os
import sys

import numpy as np

G_IM_SIZ_4b, G_IM_SIZ_8b, G_IM_SIZ_16b, G_IM_SIZ_32b = 0, 1, 2, 3
G_IM_FMT_RGBA, G_IM_FMT_YUV, G_IM_FMT_CI, G_IM_FMT_IA, G_IM_FMT_I = 0, 1, 2, 3, 4
G_TX_CLAMP = 2


def _calc_dxt(txl2words):
    return 1 if txl2words == 0 else (2048 + txl2words - 1) // txl2words


def _txl2words(width, size):
    size_bytes = [0, 1, 2, 4]
    if size == 0:
        return max(1, width // 16)
    return max(1, width * size_bytes[size] // 8)


def _reverse_dxt(val, width, size):
    if val == 0x800:
        return 1
    low = 2047 // val
    if _calc_dxt(low) > val:
        low += 1
    high = 2047 // (val - 1)
    if low == high:
        return low
    for i in range(low, high + 1):
        if _txl2words(width, size) == i:
            return i
    return (low + high) // 2


def geometry(tile_info, rice_info):
    """(width, height, bytes per row) as RT64's Rice hasher derives them, or None."""
    draw = tile_info["tile"]
    op_tex, op_tile, op_type = rice_info["texture"], rice_info["tile"], rice_info["type"]
    if op_type == "Tile":
        tw = (max((op_tile["lrs"] >> 2) - (op_tile["uls"] >> 2), 0) + 1) & 0x3FF
        th = (max((op_tile["lrt"] >> 2) - (op_tile["ult"] >> 2), 0) + 1) & 0x3FF
        tw = min(tw, 1 << op_tile["masks"]) if op_tile["masks"] != 0 else tw
        bpl = op_tex["width"] << op_tex["siz"] >> 1
        width = min(tw, op_tex["width"])
        if op_tile["siz"] > draw["siz"]:
            width <<= op_tile["siz"] - draw["siz"]
        height = min(th, 1 << op_tile["maskt"]) if op_tile["maskt"] != 0 else th
    elif op_type == "Block":
        tile_w = max((draw["lrs"] >> 2) - (draw["uls"] >> 2), 0) + 1
        tile_h = max((draw["lrt"] >> 2) - (draw["ult"] >> 2), 0) + 1
        mask_w = tile_w if draw["masks"] == 0 else (1 << draw["masks"])
        mask_h = tile_h if draw["maskt"] == 0 else (1 << draw["maskt"])
        clamps = draw["masks"] == 0 or (draw["cms"] & G_TX_CLAMP)
        clampt = draw["maskt"] == 0 or (draw["cmt"] & G_TX_CLAMP)
        width = min(mask_w, tile_w) if (clamps and tile_w <= 256) else mask_w
        height = min(mask_h, tile_h) if ((clampt and tile_h <= 256) or mask_h > 256) else mask_h
        if draw["siz"] == G_IM_SIZ_32b:
            bpl = draw["line"] << 4
        elif op_tile["lrt"] == 0:
            bpl = draw["line"] << 3
        else:
            dxt = op_tile["lrt"]
            if dxt > 1:
                dxt = _reverse_dxt(dxt, op_tex["width"], op_tex["siz"])
            bpl = dxt << 3
    else:
        return None
    return width, height, bpl


def _unswap(data):
    """Reverse each 32-bit word: RT64 dumps RDRAM little-endian."""
    n = len(data) // 4 * 4
    words = np.frombuffer(data[:n], dtype=np.uint8).reshape(-1, 4)[:, ::-1].reshape(-1)
    return np.concatenate([words, np.frombuffer(data[n:], dtype=np.uint8)])


def _rgba5551(v):
    v = np.asarray(v, dtype=np.uint32)
    r = ((v >> 11) & 31) * 255 // 31
    g = ((v >> 6) & 31) * 255 // 31
    b = ((v >> 1) & 31) * 255 // 31
    a = (v & 1) * 255
    return np.stack([r, g, b, a], -1).astype(np.uint8)


def decode(dump_dir, name):
    """RGBA uint8 array (h, w, 4) for dump texture `name` ('<hash>.v5'), or None."""
    base = os.path.join(dump_dir, name)
    with open(base + ".tile.json") as f:
        tile_info = json.load(f)
    with open(base + ".rice.json") as f:
        rice_info = json.load(f)
    geo = geometry(tile_info, rice_info)
    if geo is None:
        return None
    w, h, bpl = geo
    if tile_info.get("width"):
        w = min(w, tile_info["width"])
    if tile_info.get("height"):
        h = min(h, tile_info["height"])
    if w <= 0 or h <= 0 or bpl <= 0:
        return None
    with open(base + ".rice.rdram", "rb") as f:
        raw = _unswap(f.read())
    t = tile_info["tile"]
    fmt, siz, tlut = t["fmt"], t["siz"], tile_info["tlut"]

    palette = None
    if tlut != "None" and os.path.exists(base + ".rice.palette.rdram"):
        with open(base + ".rice.palette.rdram", "rb") as f:
            p = _unswap(f.read())
        vals = (p[0:len(p) // 2 * 2:2].astype(np.uint32) << 8) | p[1:len(p) // 2 * 2:2]
        if tlut == "IA16":
            palette = np.stack([vals >> 8, vals >> 8, vals >> 8, vals & 255], -1).astype(np.uint8)
        else:
            palette = _rgba5551(vals)

    bits = {G_IM_SIZ_4b: 4, G_IM_SIZ_8b: 8, G_IM_SIZ_16b: 16, G_IM_SIZ_32b: 32}[siz]
    row_bytes = (w * bits + 7) // 8
    rows = []
    for y in range(h):
        start = y * bpl
        row = raw[start:start + row_bytes]
        if len(row) < row_bytes:
            row = np.concatenate([row, np.zeros(row_bytes - len(row), np.uint8)])
        rows.append(row)
    data = np.stack(rows)                                    # (h, row_bytes)

    if siz == G_IM_SIZ_32b:
        return data.reshape(h, w, 4).copy()
    if siz == G_IM_SIZ_16b:
        v = (data[:, 0::2].astype(np.uint32) << 8) | data[:, 1::2]
        if fmt == G_IM_FMT_IA:
            i = (v >> 8).astype(np.uint8)
            return np.stack([i, i, i, (v & 255).astype(np.uint8)], -1)
        return _rgba5551(v)
    if siz == G_IM_SIZ_8b:
        v = data.astype(np.uint32)
    else:
        v = np.stack([data >> 4, data & 15], -1).reshape(h, -1)[:, :w].astype(np.uint32)
    if fmt == G_IM_FMT_CI and palette is not None:
        idx = np.minimum(v, len(palette) - 1)
        out = palette[idx]
        out[v >= len(palette)] = 0
        return out
    if fmt == G_IM_FMT_IA:
        if siz == G_IM_SIZ_8b:
            i, a = (v >> 4) * 17, (v & 15) * 17
        else:
            i, a = ((v >> 1) & 7) * 36, (v & 1) * 255
        i, a = i.astype(np.uint8), a.astype(np.uint8)
        return np.stack([i, i, i, a], -1)
    # Intensity: grey with the same value as alpha.
    i = (v if siz == G_IM_SIZ_8b else v * 17).astype(np.uint8)
    return np.stack([i, i, i, i], -1)


def names(dump_dir):
    """Every texture in the dump, as '<hash>.v5'."""
    return sorted(f[:-len(".tile.json")] for f in os.listdir(dump_dir) if f.endswith(".tile.json"))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.splitlines()[2].strip())
    from PIL import Image
    dump_dir, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    written = 0
    for n in names(dump_dir):
        img = decode(dump_dir, n)
        if img is None or img.size == 0:
            continue
        Image.fromarray(img, "RGBA").save(os.path.join(out_dir, n.split(".")[0] + ".png"))
        written += 1
    print(f"{written} textures written to {out_dir}")


if __name__ == "__main__":
    main()
