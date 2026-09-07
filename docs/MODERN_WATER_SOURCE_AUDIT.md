# Water geometry and course evidence

This audit uses the supplied USA Rev A reference and native replay captures.
Addresses below are cartridge virtual addresses. The new renderer keeps the
original vertices and never evaluates the game a second time for rendering.

## Original wave ownership

- `water_69D0.s`, `func_80050204`, `80050458–80050470`: the mesh builder reads
  a signed halfword from `D_80162420`, shifts it right by eight, adds
  `gWaterLevel`, and writes the vertex Y halfword. Its exterior boundary
  vertices use the mean height. The same routine derives the old highlight
  texture coordinates from neighboring height samples.
- `func_8004D628`, including `8004D924–8004D978`, reads the same signed
  fixed-point field and mean height for craft contact. It constructs a
  normalized triangular surface plane and evaluates the craft contact points
  against that plane. `func_8004E548` is the flat-water contact variant; bit
  zero in each contact point's flags records whether it is wet.
- `func_8004D30C`, `8004D30C–8004D624`, uses the same wrapped oblique grid,
  64-unit cells, fixed-point samples and mean height. Its result includes
  slope normalization; it should not be advertised as an independently
  verified exact vertical-height API. `func_8006931C` uses it to place a
  flattened craft effect at the water plane.

Thus mesh anchors and physical contacts have a common original height source.
The new material adds no geometric displacement, so it introduces no second
buoyancy surface. Original mesh quantization, interpolation, exterior flattening
and craft motion remain relevant when reviewing close contacts. Any future
subdivision must reproduce this grid and boundary behavior before adding detail.

The original flat exterior is also submitted last. In the Rev A race list
`0100B590`, 864 triangles precede six skirt quads at `0100D220`–`0100D248`.
The full and split-view lists end with the same six-quad structure. These quads
are already part of the tagged water range, not a missed second water draw.
`func_80050204` writes their boundary vertices at `gWaterLevel` and extends six
of them according to camera direction at `800509F0`–`80050AB4`.

One-player `Draw_WaterEffects` uses `AA_ZB_XLU_SURF`, which tests but does not
write depth. Modern shading returns opaque color after compositing refraction,
so retaining that state allowed a later skirt fragment to paint over a nearer
wave. A private depth copy now supplies self-occlusion for Modern replacements
of the original translucent path. Classic keeps its authored transparency;
originally opaque two-player water keeps its existing scene-depth writes.

## Submission order

The one-player race draw (`func_8009328C`) submits scene/camera setup and rider
geometry before `Draw_WaterEffects`, then `func_80069594`, `func_80068538`,
and game overlays. The two-player path (`func_800933C4`) has its own view setup.
The renderer captures scene color/depth at each identified water range and
preserves that order. It does not globally reorder transparency.

`func_80069594` uses the flattened craft transform from `func_8006931C` and
display list `010068B0`; it is not a late opaque submission of the whole course.
`func_80068538` conditionally submits dynamic craft-part lists. Both original
effects remain enabled. Their presence is a reason to retain the original
effect order rather than suppress unidentified spray or shadows.

The late `010068B0` → `01006840` craft effect is a coplanar textured triangle
with source RGB (10,10,26), using an IA16 texture and texture-times-shade
combining. It is a small dark-blue effect, not a likely bright cyan sheet.
Its inherited depth test is another reason that the new one-player water
self-depth must not be copied back into the original scene depth attachment.

Drake Lake's pre-water scene visibly contains mirrored scenery. A course-specific
optical control preserves that existing input with ripple distortion, roughness
filtering, foreground rejection and stronger grazing-angle weighting. It retains
an authored base weight because this input is LDR artwork, not scene radiance.
The captured scene is
already fogged, so its contribution is composed after the new surface fog.
This preserves authored content; it is not a new off-camera planar capture.
High otherwise uses SSR and the course environment when screen data is absent.

## Course IDs

The reference's `include/course.h` labels IDs 7 and 8 in the opposite order from
the native loaded content. The warm tropical course is 7 and the icy course is
8. The original selection ordering (`D_800DAAD8` at `800DAAD8`) is
`0,1,2,4,3,5,6,8,7`; the championship tables in `code_4C750.c` also place 7 last
and introduce 8 on Expert. Profiles follow native content:

| ID | Profile |
| --- | --- |
| 0 | Dolphin Park |
| 1 | Sunny Beach |
| 2 | Sunset Bay |
| 3 | Marine Fortress |
| 4 | Drake Lake |
| 5 | Port Blue |
| 6 | Twilight City |
| 7 | Southern Island |
| 8 | Glacier Coast |
| 9 | Rider selection |

The QA course override runs before the original race initializer. The asset
loader at `80095050` subsequently selects its DMA tables using that course ID;
the test does not merely relabel an already loaded course. Original and modern
comparisons use the same override and deterministic input.

## Stable shoreline geometry

`func_8007F448` queries the course's spatial collision grid. `func_80071E70`
installs its grid, list and bounded-plane pointers at `801C3B7C`, `801C3B80`
and `801C3B84`. Each plane occupies 64 bytes: an origin, surface normal,
three boundary normals and a final clipping distance. The four boundary
tests define a finite polygon. The raw course collision DMA uses segment 6,
loaded at `80306800` (`D_800D45EC`); it is separate from compressed visual
geometry. The course AABB/out-of-bounds lists are not shoreline geometry.

The game-thread snapshot now collects only plane IDs referenced by that grid,
checks the original per-course pointer tables and address bounds, and intersects
valid finite planes with the current mean water height. It rebuilds on course,
pointer, generation or mean-height changes. No collision function is called,
no RDRAM is written, and no live pointer is retained by RT64.

Each craft tile receives at most twenty nearby segments, ordered by world
distance. This bounded budget keeps the shared draw structure below D3D's
2048-byte stride limit. The fixed-step compute pass evaluates a local distance
mask and retains shoreline wash in a separate field channel. That channel
reprojects in world space and decays independently from wind-advected wake
foam. Emission fades before the tile boundary and the surface contribution
fades above/below the mean-water band. It is a local, budgeted shoreline field,
not a claim that the original collision mesh covers every visible coastline.
Missing or invalid geometry contributes nothing; screen-depth contact shading
continues to cover local objects and unrepresented edges.

`tools/verify_water_shore.cpp` checks a known triangle's intersection, changing
water levels, endpoint distances and invalid input. The native diagnostic
(`F10`, view 11) shows craft wake in red, persistent shoreline wash in green,
and crest/depth contact in blue. Optional field statistics record shoreline
coverage, mass and peak separately from wake foam.
