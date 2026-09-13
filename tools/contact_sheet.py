#!/usr/bin/env python3
"""Tile captured frames into one labelled image, to read a few frames at a glance.

    python tools/contact_sheet.py OUT.jpg COLS SCALE [--crop X0 Y0 X1 Y1] FRAME [FRAME ...]
    python tools/contact_sheet.py OUT.jpg COLS SCALE --dir DIR --from MS --to MS [--every N]

Each frame is cropped to the box (in the saved frame's pixels), scaled, labelled
with its file name, and placed left to right, top to bottom. --dir picks the
frames tools/capture_frames.py saved between two times since launch.

The crop is what makes a split-screen defect visible: a two-player frame is two
views stacked, and a flicker in one view is invisible at the scale a whole frame
has to be shown at. Needs Pillow.
"""
import argparse
import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("needs: pip install pillow")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("cols", type=int)
    ap.add_argument("scale", type=float)
    ap.add_argument("frames", nargs="*")
    ap.add_argument("--crop", type=int, nargs=4, metavar=("X0", "Y0", "X1", "Y1"))
    ap.add_argument("--dir")
    ap.add_argument("--from", dest="start", type=int, default=0)
    ap.add_argument("--to", dest="end", type=int, default=1 << 30)
    ap.add_argument("--every", type=int, default=1)
    args = ap.parse_args()

    files = list(args.frames)
    if args.dir:
        names = sorted(n for n in os.listdir(args.dir) if n.startswith("t") and n.endswith(".jpg"))
        picked = [n for n in names if args.start <= int(n[1:-4]) <= args.end][::args.every]
        files += [os.path.join(args.dir, n) for n in picked]
    if not files:
        sys.exit("no frames")

    tiles = []
    for path in files:
        image = Image.open(path)
        if args.crop:
            image = image.crop(tuple(args.crop))
        image = image.resize((max(1, int(image.width * args.scale)), max(1, int(image.height * args.scale))), Image.LANCZOS)
        label = os.path.basename(path)
        draw = ImageDraw.Draw(image)
        draw.rectangle((0, 0, 8 * len(label) + 6, 16), fill=(0, 0, 0))
        draw.text((3, 2), label, fill=(255, 255, 0))
        tiles.append(image)

    width, height = tiles[0].size
    rows = (len(tiles) + args.cols - 1) // args.cols
    sheet = Image.new("RGB", (width * args.cols, height * rows), (40, 40, 40))
    for i, tile in enumerate(tiles):
        sheet.paste(tile, ((i % args.cols) * width, (i // args.cols) * height))
    sheet.save(args.out, quality=88)
    print("%s: %d frames, %dx%d" % (args.out, len(tiles), sheet.width, sheet.height))


if __name__ == "__main__":
    main()
