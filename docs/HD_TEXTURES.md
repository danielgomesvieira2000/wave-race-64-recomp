# Bundled HD textures and reconstruction workflow

The renderer supports a separate RT64 replacement pack through **Settings →
Graphics → Textures → Original / HD**. HD is the default. Changing the
selection during play updates the renderer without restarting the game.
Unmatched textures use the original ROM artwork.

The reviewed pack is included in `assets/textures/nano-banana-2` and bundled by
the build. No separate installation is needed. The default soundtrack is Custom
and default water is High with Modern appearance; saved explicit choices are
respected.

On macOS, an optional user-installed pack (containing `rt64.json` and `textures/`)
overrides the bundled pack at:

```
~/Library/Application Support/WaveRace64Recomp/textures/nano-banana-2/
```

Source builds include the reviewed runtime replacements, their content manifest
and credits. ROM dumps, original texture captures and generation work sheets
remain local. The texture tools write working data under ignored `build*/`
directories. Never copy account responses, signed download URLs or local-path
review logs into the published pack.

Install the image-tool dependencies into the checkout's virtual environment
with `.venv/bin/python -m pip install -r tools/textures/requirements.txt`.

## Capture and source coverage

`WR64_TEXTURE_DUMP=/absolute/output/path` enables RT64's original TMEM and tile
metadata capture before the first display list. `WR64_TEXTURE_PACK=/absolute/pack`
loads a specific pack and forces HD for that process; it does not change the
saved menu selection. Omit the override when testing the menu toggle.

`tools/textures/decode_tmem.py` decodes the dumped TMEM using RT64's texture
sampling layout, including palette variants, odd-row addressing, and RGBA32
banks. Images deduplicate by their decoded pixels; all exact RT64 version-5 hash
aliases are retained. Unsupported or incomplete records are reported as errors.

`capture_matrix.py` covers all nine courses in one- and two-player modes.
`capture_coverage.py` adds all sixteen rider texture banks and seven menu pages.
The menu fixtures release scripted inputs before opening a page, so inspecting
the erase/save pages does not perform an erase or save operation.

The broad capture runners deliberately force Original water at 30 Hz to reduce
capture cost. Their windows are source-capture tools, not the normal play build.
For a visual acceptance run, explicitly select the intended water quality and
style, or use `run_water_replay.py --saved-quality --saved-appearance` to verify
saved settings. The `appearance: Modern` log reports style only; confirm that
the water pass runs with a nonzero quality in a profiling trace. Relaunch the
normal app without QA overrides when handing it back for play.

`inventory_rom.py` and `extract_static.py` supplement runtime captures with
authored display-list loads from the USA Rev A ROM. The static extractor follows
bounded display-list calls, tracks palette and segment state, and uses the
actual RT64 hash implementation. Confirmed hashes and conditional,
format-supported variants are identified separately. A loaded texture bank or
short course replay does not establish that every possible gameplay state has
been rendered.

The initial source audit contained 1,613 distinct original images and 1,635 RT64
aliases. Of those, 1,293 images / 1,309 aliases were observed in native captures,
including an additional pause-screen pass.
Remaining mappings come from complete authored source loads. These are the
initial inventory counts, not a claim that every possible dynamic texture or
gameplay state has been exercised. Later focused UI and course captures add
sources; each assembled pack records its current mappings and source inventories
in `pack.json` rather than treating the initial count as complete coverage.

## Generation and reconstruction

The Nano Banana 2 workflow uses `imagen-nano-banana-2-flash`, one image per
request, with a 4:3 reference sheet. The broad pass uses 4K sheets; focused
terrain and portrait passes use 2K sheets. Generation is an external, paid step;
preparing or assembling sheets never makes generation requests.

1. Join contiguous fragments using their authored addresses and exact crop
   regions. Wide interface panels and word fragments are processed together.
2. Prepare 1024×768 reference sheets with `atlas.py prepare`. Use 8×6 cells for
   small assets, 4×3 for larger images, and 2×1 or 1×1 for wide panels. Preserve
   original RGBA and sampler padding separately from the reference image.
3. Upload each source sheet, quote the actual model cost, and record the request
   before generation. Record the returned creation identifier before proceeding.
   Resume pending creations instead of submitting duplicates.
4. Download completed sheets and retain them with the manifests. Assembly uses
   normalized crop coordinates because returned image dimensions can differ
   from the requested aspect and size.
5. Assemble at exactly four times each original texture's dimensions. Preserve
   alpha, low-frequency source colors, seams, and native color boundaries.
   Visually inspect source, raw candidate, and reconstructed output side by side.
6. Reject invented letters, logos, faces, mechanical features, and other changes
   to the artwork through an explicit per-image policy. A source-preserving
   resize is reported separately from accepted generated detail. Metrics alone
   cannot approve a texture.
7. Handle lettering as a separate reviewed pass. Retain original word layout,
   spacing, color gradients, and hash assignments; join split words before
   reconstructing them. The local font pack uses focused Nano Banana 2 redraws
   for colored HUD labels and digits, plus vector reconstruction for audited
   white labels and alphabet glyphs. Character identities and advances are
   checked against ROM tables; special symbols retain their source design.
   `refine_fonts.py` records the explicit typeface path, index, and file hash.
   The local vector pass uses Helvetica Bold; no system font file is bundled.
   Fit crisp ink to the source's solid coverage, rather than its faint alpha
   fringe. Leave a half-native-texel transparent guard at the outside of a whole
   word before splitting it into strips; otherwise clamped filtering can extend
   opaque terminal rows into visible marks below the letters.
8. For cyclic scrolling images, generate a common periodic parent and crop every
   phase from the same texels. `periodic_y` is validated against the source and
   enforced in the reconstructed output to prevent frame-to-frame detail changes.

`merge_packs.py --pack ... --pack ... --inventory ... --output ...` applies later
reviewed packs as explicit overrides, then fills missing aliases from the same
canonical source image. It validates source pixels, region dimensions, output
scale, and conflicting alias assignments before writing the merged database.
The default remains exactly 4×. Use `--allow-scale 8` only for explicitly
reviewed 8× sources; mixed horizontal/vertical scales are rejected.

The two UI logo variants use larger artwork from the
[LaunchBox image catalog](https://gamesdb.launchbox-app.com/games/details/392-wave-race-64-kawasaki-jet-ski).
The local provenance records the downloaded file and its SHA-256. Fit the oval
and Kawasaki strip separately to their original occupied bounds, then split
the joined result into the original 33 texture strips. Preserve these as PNGs
without independent strip mipmaps. The Dolphin Park arch is reconstructed as
one 128×64 source image before a focused redraw, then cut into its three authored
UV regions; the alternate left texture retains its unused original padding.

## Filtering and native validation

`build_mip_pack.py` converts world-material PNGs to uncompressed RGBA8 DDS with
full mip chains. The top level preserves the PNG pixels exactly; lower levels
use premultiplied area filtering. A policy can retain PNGs for lettering and
interface elements. `tools/tests/ddspp_probe.cpp` verifies the files using the
same DDS parser as RT64.

The tracked `tools/patch_texture_packs.py` reproduces the frontend application
accessor and texture sampler fix against the pinned dependencies. The sampler
clamps mip selection before integer conversion and handles zero UV derivatives,
preventing invalid trilinear weights. `tools/build_macos.sh` applies this patch
before building. No submodule revision changes are required.

Validate the final pack in the packaged native app, including readable HUD and
menu text, moving world textures, scrolling variants, split-screen play, and an
Original/HD toggle. Run bounded fixtures with `tools/run_water_replay.py`, retain
their runtime logs and exit results, and inspect the exact bundle intended for
delivery. A successful assembly or build is not visual acceptance.

Focused checks can be run with:

```
.venv/bin/python -m unittest tools.tests.test_tmem_decoder \
  tools.tests.test_extract_static tools.tests.test_texture_atlas \
  tools.tests.test_merge_packs tools.tests.test_texture_periods \
  tools.tests.test_mip_pack tools.tests.test_texture_capture_coverage \
  tools.tests.test_texture_sampler_patch tools.tests.test_refine_fonts
```

## Completing UI families

A captured glyph or icon does not imply that the whole family was captured. The
white fonts have separate 8×8, 16×12 and 24×20 glyph sets, including punctuation,
and course/result headings can be whole images. Complete each original ROM
array, then calibrate its texture hashes against captured members using the
actual RT64 v5 hasher. Keep unobserved hashes marked conditional until a native
capture confirms them; do not infer aliases from visual similarity alone.

The power meter has 20 RGBA32 pennants: five colors and four brightness states.
Use one registered silhouette across all states, but preserve each authored
palette and brightness. MAX POWER is a separate 144×20 IA8 image whose white
fill is tinted by the game. It needs its own mapping and must stay grayscale.

The MISS display uses two separate 12×12 RGBA32 buoy icons: the normal glossy
red sphere with a yellow tip, and its darker crossed-out state. Reconstruct both
at 8×, fit their original occupied bounds, and retain transparent guards. Keep
these small HUD images as PNGs so independent mip selection does not soften them.

The small rank indicator is a separate 8×10 IA8 upward arrow. Its color is
supplied by the game; it is not part of the rank number texture. Reconstruct it
at 8× with a symmetric head and stem fitted to the original solid white bounds,
while keeping its transparent guard and grayscale coverage.

The race HUD portrait table contains eight opaque 20×20 images: four riders
and four alternate helmet palettes. Process the complete table together, then
fit each 8× portrait to its original occupied bounds and black background.
Check visor coverage, helmet paint stripes and checker patterns against the
source. The green, red and finish-checker frames are separate textures; changing
portrait artwork must not alter frame dimensions or their UVs. Preserve the
portraits as PNGs without independent mipmaps.
The current local pack uses the user's selected eight-portrait sheet with dark
visors. Crop out the atlas gutters and fit the chosen artwork uniformly into
the original footprints; the selected designs supersede the preliminary
source-pattern corrections. Record the supplied file hash and exact crop
rectangles separately from generated candidates.

Large result overlays and multi-part words must be joined before redraw and
split along their original texture boundaries afterward. Preserve PNG storage
for these strips. The small title-screen legal line is a 256×6 IA8 image; a
hard alpha threshold destroys its thin letters, so use an audited transcription
and a fitted vector font rather than thresholding its coverage.

For the number-2 craft decal, the original model's texture T increases upward
in model space: the stored numeral looks vertically inverted. Restore it using
an upright reference, then invert it back for the unchanged UVs. Restrict any
redraw to the registered badge mask; the surrounding paint reaches model seams
and must retain the original boundaries. Cover both primary and secondary
model texture identities.

Single-player races select the secondary model banks for the three opponents;
this is a model-loader choice, separate from DDS mip selection. The bank table
contains four riders, four alternate palettes, and their eight secondary banks.
Most secondary craft textures have half the primary texture's dimensions.
Audit these sources separately: a 4× replacement of a secondary source can
still contain considerably less detail than the player's primary material.
Some secondary meshes adjust their texture coordinates, so derive replacements
per material with source-layout checks rather than redirecting every secondary
hash to the corresponding primary texture.

The initial craft audit found 279 mapped aliases, including 125 images whose
installed top mip was an exact source resample. An existing mapping or a larger
file alone does not establish sharper art. Compare original, installed top mip
and raw generation before refining a material. Protect incorrect generated
numerals locally; a broad source-gradient mask can erase almost all useful
paint detail along with the unwanted lettering.

The focused craft supplement covers 32 side-panel textures across all four
riders and both color sets. Primary sources remain 4× and their matched
secondary sources become 8×, giving both models 256×128 paint panels. Retain
each secondary source's own hash, registration and seam guard. Badge identities
are 2/4/1/22 for the normal paint sets and 3/8/7/95 for their alternates. Preserve
the native vertical UV orientation and the correct light/dark disk polarity.
Other craft materials retain their earlier reviewed replacements; this focused
supplement is not a claim that every original fallback now contains new detail.

Ramp wood uses four 64×32 RGBA16 sources: two board-grain/color variants, a
painted-arrow board, and paired end grain. All four repeat with S/T masks 6/5;
the two plain variants are not separate levels of detail. A replacement can
load successfully yet still look soft when reconstruction retains only resized
source detail. Review the actual top mip against the source, then restore fine
grain while registering plank divisions, arrow geometry, paint, and repeat
edges. The focused ramp pass uses 8× materials with complete DDS mip chains for
close views and stable filtering at a distance.

For island terrain, establish material usage from the original mesh and UVs
before choosing a redraw. Dolphin Park's green cap repeats in both directions;
its vegetation/earth slopes, cliff edges, and sand strip repeat horizontally
and clamp vertically. Preserve the ordered material bands and original edge
samples. Fine detail can be retained while correcting coarse palette drift
and recoloring only misplaced green, earth, or submerged regions. The local
`refine_island_terrain.py` compositor records these corrections per texture.
Reject generated checker or mosaic artifacts in sand; a previously reviewed
candidate can be reused with its original generation and crop provenance.
