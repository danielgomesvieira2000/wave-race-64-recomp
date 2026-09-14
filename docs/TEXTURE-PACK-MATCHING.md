# Texture pack matching

Turning a texture pack made for another emulator into a pack this port loads, when the
pack's hashes cannot be computed from the N64's data. Two tools:

| Tool | Does |
|---|---|
| `tools/rt64_texture_dump.py` | Decodes an RT64 texture dump into RGBA images. Importable. |
| `tools/match_texture_pack.py` | Matches a Dolphin-named pack against a decoded dump by picture and writes a mod directory, a report and review sheets. |

The first half of this document is how to use them. The second half is how they work,
in enough detail to build the same thing for another game or another port.

---

## Using it

### What it is for

A pack made for Wave Race 64 on the **Wii Virtual Console under Dolphin** names its
images the way Dolphin does:

```
tex1_64x32_21047e97ba2434d0_39e9f59e81e4bfeb_9.png
     |     |                |                 `-- GameCube texture format (5 RGB5A3, 9 C8, 3 IA8, 2 IA4, 6 RGBA8, 0 I4)
     |     |                `-- palette (TLUT) hash, colour-indexed textures only
     |     `-- Dolphin's hash of the texture the Virtual Console's emulator converted
     `-- the texture's own size
```

RT64 looks textures up by its **own** hash of the N64 texture memory (`hash version 5`).
The Dolphin hash is of data that only exists inside the Virtual Console's emulator, so no
arithmetic turns one into the other. The picture carries over: `64x32` is the N64
texture's size, and the image is an upscale of it. So the pack is matched by picture.

Rice/GLideN64 packs (`GAME#crc#fmt#siz_all.png`) do not need this: RT64 matches Rice
CRCs itself (`texture_hasher --rice`, `autoPath: rice`).

### 1. Dump the game's textures

**F1** → **Textures** → *Start dumping textures*. Play everything the pack covers --
every course, the menus, championship, time trials, stunt mode, 2P, the results screens
-- then *Stop*. **The dump decides the coverage**: a texture the dump never saw cannot be
matched. RT64 writes the dump under the settings folder.

### 2. Match and build the mod

```
python tools/match_texture_pack.py <pack dir> <dump dir> <mod dir> --id my_pack --name "My Pack" --author Name --sheets <dir>
```

Output, measured on Nicolas's pack (2,372 images) against a 1,542-texture dump, ~75 s:

```
dump: 1542 textures decoded
pack images: 2372 -- colour pass: sure 658, review 241, none 1473 (318 with no dump texture of their exact size)
replaced: 940 RT64 textures from 940 images (136 from review, 184 from the shape pass); 602 dump textures left as they are
```

| Written | What |
|---|---|
| `<mod dir>/mod.json`, `rt64.json`, `textures/` | The mod: only the images used, keyed by RT64 hash. |
| `match_report.json` (beside the mod dir) | Every pack image: size, colour error, correlation, alpha diff, runner-up, verdict, the RT64 hashes, `lost_to` when another image won its texture, and `shape_match` when the shape pass placed it. |
| `--sheets <dir>/sure_sample.png` | 48 random accepted `sure` pairs. |
| `--sheets <dir>/review_accepted.png` | Every accepted `review` pair. |
| `--sheets <dir>/shape_accepted.png` | Every shape-pass pair. |

### Why not every pack image is placed

Measured on Nicolas's pack, which has **2,242 distinct pictures** (2,372 names):

| | Pictures |
|---|---:|
| Placed by the colour pass | 756 |
| Placed by the shape pass | 184 |
| A texture the dump never captured | most of the rest -- e.g. 367 unused 64x32 and 262 unused 32x32 pictures against 55 and 81 unmatched textures of those sizes |
| Colour variants of one texture (format C8: 832 unused) | a rider's or a buoy's colour scheme the dump did not see drawn |

A fuller dump is the lever: every texture it adds is a candidate.

### 3. Look at the sheets

Each pair is the pack image, then the dump texture, on magenta where transparent, with
the colour error (`e`) and correlation (`c`). Wrong pairs look wrong at a glance: a
different colour scheme, a different letter, a logo against a plain strip. If one gets
through, find it in `match_report.json` and tighten the threshold that let it in (see
*The rules* below).

### 4. Pack and install

```
python tools/pack_mod.py <mod dir> --install
```

Run the game from a terminal; the chain ends in `RT64: 1 texture pack(s) loaded by the
port.` **F4** toggles replacement, to see what the pack changes. See
[../examples/mods/README.md](../examples/mods/README.md).

### Decoding a dump to look at it

```
python tools/rt64_texture_dump.py <dump dir> <out dir>
```

writes every texture as `<hash>.png` -- 1,542 in under a second here.

### Redistribution

The pack's images belong to their author, and a dump is the game's own data. Build these
for local use. Nothing from either belongs in this repository; see
[../CONTRIBUTING.md](../CONTRIBUTING.md).

---

## Putting it in another project

### The RT64 dump, and decoding it

A v5 dump is five files per texture, named `<rt64 hash>.v5.<kind>`:

| File | Contents |
|---|---|
| `tile.json` | `tile` (`fmt`, `siz`, `line`, `uls/ult/lrs/lrt`, `masks/maskt`, `cms/cmt`, `palette`, `tmem`), `tlut` (`None`/`RGBA16`/`IA16`), `width`, `height` |
| `tmem` | RT64's 4096-byte TMEM image |
| `rice.json` | The load operation: `type` (`Tile`/`Block`), `texture` (`address`, `fmt`, `siz`, `width`), `tile` |
| `rice.rdram` | The RDRAM the texture was loaded from |
| `rice.palette.json`, `rice.palette.rdram` | The palette, colour-indexed textures only |

There is no image. **Decode the Rice files, not TMEM.** TMEM has RT64-internal layout:
RGBA32 split across its two halves, odd rows word-swapped, palette entries 8 bytes each
at `0x800` (`rt64_tmem_hasher.h`). The Rice files are N64 memory as the game loaded it.

**Size and row stride**: transcribe `addRiceHash()` in
`lib/RT64/src/tools/texture_hasher/texture_hasher.cpp` exactly -- the `Tile` branch
(`bpl = texture.width << siz >> 1`, width and height from `lrs/lrt` and the masks) and the
`Block` branch (clamp and mask rules, `bpl` from `line` or from `ReverseDXT(lrt)`). Then
cap width and height at `tile.json`'s. Guessing the stride from `line` alone decodes
some textures and shears the rest.

**Byte order**: `rice.rdram` is written as **little-endian 32-bit words**. Reverse every
4 bytes, then read texels big-endian. The symptom of forgetting: text scrambled in
4-byte runs ("SUNNY BEACH" unreadable), most textures noise, a few plain ones fine --
which reads like a stride bug and is not.

**Formats** (`fmt`, `siz`):

| | 4b | 8b | 16b | 32b |
|---|---|---|---|---|
| RGBA (0) | -- | -- | RGBA5551 | RGBA8888 |
| CI (2) | index nibble, TLUT | index byte, TLUT | -- | -- |
| IA (3) | I3A1 | I4A4 | I8A8 | -- |
| I (4) | I4, alpha = I | I8, alpha = I | -- | -- |

TLUT `RGBA16` entries are RGBA5551, `IA16` are I8A8, from `rice.palette.rdram` (same
word swap). CI4 packs its two indices high nibble first.

**Check the decode by eye before matching anything.** A random sheet of 48 decoded
textures: menu words, digits and the N64 logo must read correctly.

### Matching

For each pack image:

1. **Shrink** to the size in its name: premultiply alpha, box-filter resize. Straight
   RGB resizing bleeds the colour of transparent pixels into edges.
2. **Colour error** against every dump texture of that size: mean |Δ| of premultiplied
   RGBA, 0-255. Cheap as one array operation per size.
3. **Re-rank the five lowest** by luminance correlation: one within 1.5× the best error
   with correlation 0.05 higher wins. Colour error alone confuses near-flat textures,
   and the Virtual Console's palette conversion shifts colours while keeping shape.
4. **Runner-up** = the lowest error among candidates whose decoded picture is
   *different* from the winner's. The same picture under other tile parameters has
   another RT64 hash and is not a rival: map the image to all of them.

### The rules, and where they came from

Thresholds were set from contact sheets across error bands, not guessed:

| Band (colour error) | What the sheets showed |
|---|---|
| < 10 | Correct, except near-featureless textures (a transparent sliver, a flat grey), which match anything |
| 10-20 | Mostly correct; look-alike panels wrong |
| 20-35 | Right shape, wrong variant (a rider's helmet or a sponsor logo in another colour scheme), or no counterpart |
| > 35 | No counterpart in the dump |

Hence:

- **sure**: clear of the runner-up (1.25× or +4), correlation ≥ 0.90, alpha diff < 16,
  and error < 10, or < 25 with correlation ≥ 0.93. **Featureless** (luminance and alpha
  std both < 8): error < 3. The first version without the correlation floor and the
  featureless rule let 3 of 40 sampled pairs through wrong (a letter onto a pink blob,
  an empty texture); with them, 48 of 48.
- **review**: error < 35, correlation ≥ 0.85. **Accepted** only below error 20 and
  correlation 0.905; the one bad pair among 142 at 0.90 was a plain strip matched to a
  strip with a logo.
- **One image per texture**: group accepted images by the dump picture they matched and
  keep the lowest error. Animation frames and colour variants otherwise all claim the
  same texture, and the last one written wins at random.

### The shape pass: padding and redrawn art

The colour pass left two whole classes unplaced, found by accounting for every unused
picture rather than by tuning thresholds:

- **Padded sizes.** Every Dolphin name has dimensions that are multiples of 4; 62 of the
  dump's 119 texture sizes are not (80x3, 96x5, 160x5, 8x1, 40x10...), covering 290 of
  its 1,542 textures. The unmatched game sizes line
  up with unused pack sizes padded: 79 textures at 80x3 against 60 pictures at 80x4, 19 at
  96x5 against 28 at 96x8, 16 at 160x5 against 16 at 160x8. **Where the content sits**,
  measured by best alpha correlation over 184 candidate pairs: top-left 0.62 median,
  centre 0.37, stretched to fill 0.40, bottom 0.24. So: crop the top-left `W×H` fraction
  of the image, then shrink.
- **Redrawn lettering.** Menu and HUD text strips ("CANCEL", "DRAKE LAKE", "PRESS START")
  match the right texture at a colour error of 40-60: crisp redrawn glyphs shrunk to 10
  pixels do not reproduce the original's blocky edges. Shape agrees; pixels do not.

**Scores**, at native size: `shape` = mean of luminance correlation and alpha correlation
(luminance alone when either side's alpha standard deviation is ≤ 5).

**Rule**: the pack image and the dump picture are each other's best shape match, shape ≥
0.70, the pack image's margin over its next picture ≥ 0.02, and **margin ≥ 0.10 or
colour error < 25**. The pass only fills pictures the colour pass left empty.

**Calibration**, against the colour pass's 756 matches: at every setting tried (shape 0.70
to 0.95, margin 0.02 to 0.10) the mutual-best rule **contradicted none** of them, and 711
of the 756 are mutual best with no threshold at all. The new pairs were then reviewed by
eye in score order. The mistakes all had a small margin with a high colour error -- red
bars against grass (margin 0.03, error 30), a white streak against a waterfall (0.02, 92),
a fire gradient against a red strip (0.07, 34) -- which is the last condition.

**Tried and rejected: a coarse colour check** (mean colour over 4×4 blocks, error < 20)
to catch same-shaped colour variants. It dropped 92 of 190 candidates, nearly all correct
lettering whose clean redraw differs in average colour from the original's anti-aliasing,
and removed nothing the margin/error condition did not.

**Where shape fails: colour variants.** Luminance correlation cannot tell a red livery
from a blue one, or one rider's helmet from another's. Against the dump that does not
arise -- the rule picks the texture with the same colours through the mutual-best test
and the error condition. Matched against another pack's upscaled images instead (below),
it does.

### Matching through another HD pack (not automated)

For textures the dump never captured, an existing HD pack keyed by RT64 hash can stand in
for the original: its images are upscales of the same textures. Measured with the HD pack
from Elliott Tate's fork (1,828 hashes, 420 not in the dump):

- Its upscale factors are exactly **4× (1,349) or 8× (59)**, so each texture has one or two
  candidate native sizes.
- **The colour pass's rules**, applied to its images at native size and scored on the 710
  textures both routes can place: 96% agreement; of 22 disagreements, 19 were the same
  artwork at the other size or the mirrored half of a two-sided texture. 113 of the 420
  placed.
- **The shape pass's rule** placed 123, but reviewed by eye about half of the new ones were
  the wrong colour variant (a red livery for a blue one). Not used.

The merge was done with one-off scripts, not this tool.

### Writing the pack

`rt64.json` with the configuration the port's other packs use --
`{"autoPath": "rt64", "configurationVersion": 3, "hashVersion": 5, "defaultOperation":
"stream", "defaultShift": "half"}` -- and one `{"hashes": {"rt64": "<hash>"}, "path":
"textures/<file>"}` per hash. Several hashes may share a file. The hash is the dump file
name without `.v5`. RT64 reads the pack straight from the `.nrm` zip.

### What it cannot do

- **Textures the dump did not capture** stay unmatched. Dump more, run again.
- **A redrawn image** whose shape no longer resembles the original falls below the
  shape floor.
- **A colour variant the dump never drew** has no texture to match, and shape matching
  cannot supply one from an image of another variant.
- **Two genuinely different textures that look the same at native size** (animation
  frames a pixel apart) may swap. They look the same, so it rarely shows.

---

## Keeping this current

When either tool's thresholds, output or options change, change this file in the same
commit. RT64 dump format facts also go in [PORTING.md](PORTING.md); the mod format in
[../examples/mods/README.md](../examples/mods/README.md).
