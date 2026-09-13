# The modern water renderer

An optional replacement for how Wave Race 64's water is *shaded*. The waves
themselves are still the cartridge's: the same lattice, the same simulation, the
same heights, so the craft handles identically at every setting. What changes is
what happens to that surface once the game has built it -- lighting, colour with
depth, refraction, wakes that persist, wash along the shore.

Two halves: [**using it**](#using-it) and [**putting it in another
project**](#putting-it-in-another-project).
[WATER-IMPLEMENTATION.md](WATER-IMPLEMENTATION.md) is the full account of how
the fork built it and where this repository differs.

**Best quality with the Aqua style is the default**, on every platform -- what the fork
this came from shipped, and what the water was tuned against. It is not free:
expect the time spent drawing a frame to roughly double, see [What it
costs](#what-it-costs). **Original** is the first entry in the Water tab and is
a true bypass, not a degraded mode -- on that setting the renderer is not
engaged at all and the water is exactly the cartridge's.

The renderer comes from [PR #2][pr] by elliotttate, which built and played it on
an Apple M3 Max. This tree has since verified it on Windows and D3D12, which
that branch explicitly had not.

[pr]: https://github.com/danielgomesvieira2000/wave-race-64-recomp/pull/2

---

## Using it

### The settings

Everything lives in the **Water** tab, which sits beside Graphics. It is one
decision and then refinements of it: **Water quality** is a three-step ladder,
and every option below it is greyed out while it would do nothing, or hidden
when it belongs to a different style.

| Water quality | What it does |
|---|---|
| **Original** | The game's own water. The renderer is not engaged, nothing below has any effect, and every option below is greyed out. |
| **Enhanced** | Sun and sky lighting, colour that deepens with the water, refraction, persistent wakes behind each craft, wash along the shoreline. |
| **Best** *(default)* | Enhanced, plus screen-space reflections of scenery in view, and fine airborne spray. |

| Option | Shown | What it does |
|---|---|---|
| **Water style** | Enhanced, Best | Least to most departure from the cartridge. **Classic** keeps its own colours, transparency, fog and broad highlights and adds only the effects. **Deep** is richer and darker. **Aqua** *(default)* is Deep with a lighter teal and clearer shallows. |
| **Clarity** | Deep, Aqua | 50% is each style's usual look. Below it the water turns murkier; above it the water shows more and more of what the game draws under the surface. See [Clarity](#clarity). |
| **Aqua brightness** | Aqua | 0.4x to 2.5x the water's own colour; 50% is the usual look. |
| **Aqua tint** | Aqua | Deep blue at 0% to green turquoise at 100%. |
| **Surface ripples** | Enhanced, Best | Soft / **Normal** / Strong. Fine detail added on top of the game's waves, never replacing them. |
| **Spray** | Best | The *added* airborne spray. Off keeps the surface foam, the wakes and the game's own splashes. |

The menu names are not the names in the code, the saved settings, the
environment overrides or the log, which kept the fork's:

| Menu | `water.json`, `WR64_WATER*`, log | Code |
|---|---|---|
| Original / Enhanced / Best | `original` / `modern` / `high` | `Quality::Original` / `Modern` / `High` |
| Classic / Deep / Aqua | `classic` / `modern` / `aqua` | `Style::Classic` / `Modern` / `Aqua` |
| Clarity | `aqua_clarity` | `set_clarity` |

The menu was renamed in 0.9.2: the quality was Original / Modern / High and the
style Classic / Modern / Aqua, so "Modern" meant two different things on one
tab. The ids did not change, so saved settings read as they were.

### Clarity

Two different things, one either side of 50%:

| Slider | What changes | Where it shows |
|---|---|---|
| **0-50%** | absorption up to **x6**, visibility range down to **x0.4** | shallow water: shores, ramps, around the piers. Over open sea little of the bottom was visible to begin with, so little changes |
| **50%** | nothing: Deep as authored, Aqua with its fixed step (absorption x0.6, visibility x1.4) | |
| **50-100%** | absorption down to **x1/3**, visibility up to **x1.29**, and the **see-through weight** rises from the course's `authored_reflection` to **1** | everywhere in one-player: the sea floor, fish and the dolphin show through, and the water comes back toward the cartridge's own colour |

The see-through weight is the profile field `authored_reflection` (see [Course
profiles](#course-profiles)). It blends the finished water toward the image the
game had drawn *before* the water: the sea floor, anything swimming, and the
course's own water colour. Pushing absorption and visibility alone does not get
there, however far: the shader lights and darkens what it refracts as part of
the water body, so the bottom stays dim.

| Limit | Why |
|---|---|
| **One player only** above 50% | in two-player races the cartridge draws its water opaque, so the image under the surface is that flat water. Tried on a two-player Sunny Beach race: the ripples and lighting flatten into a pale sheet. The lower half still applies |
| Fades within **15-70 units** of the mean water height | inherited from the shader's reflection term; only tall swells are affected |
| Visibility still capped at **140** (or the profile's own range if higher) | past the course's underwater geometry the shader must resolve to water, never to exposed sky |
| Hidden under **Classic** | Classic's shader rebuilds the colour from the cartridge's own image and reads none of these values |

**A saved value away from 50% looks different since 0.9.2.** The first ranges
were too weak to see -- brightness 0.65-1.35x and tint +-12-20% came out as
roughly +-15% and +-5% on screen once tone mapping and gamma had compressed them,
which is what made the three sliders read as inert. Brightness 70%, for example,
was 1.14x and is now 1.44x; Clarity 80% now includes a 60% see-through blend.

### How far the sea reaches

The game animates a patch of water **922 units** across, carried with the camera,
and that is all the water it draws. Everything beyond it is painted onto the
lowest of the sky's three bands -- a seven-vertex plane at sea level, flat. Which
means the shading stops in a ring around the player, and so does the sea.

The port draws the missing surface itself: a ring of quads from the patch's edge
out to the distance the rest of the course is drawn at, **standing on the same
wave field the game's own patch stands on**, so the swell outside agrees with the
swell inside rather than being invented. Its heights come from the game's own
height query, sampled on the game thread.

It carries the game's own water attributes too -- texture coordinates of
`(1024, 1024)` and a shade of `FFFFFFB0`, read out of a trace of the patch. The
alpha is the part that matters: the water is drawn translucent, and an opaque
ring would read as a solid band laid over the sea. That is what lets this work
with **Water at Original** as well as with the modern renderer -- there it is
simply the game's water reaching further, with the game's own texel and
translucency.

| | |
|---|---|
| When it is drawn | whenever **either** Water quality or Draw Distance is above Original |
| How far | as far as Draw Distance asks for, or the course's own cull, whichever is further |
| Size | 9 circles x 48 sectors: 432 vertices and 384 quads a frame, against the game's own 500 vertices |
| Turning it off | `WR64_NO_WATER_RING=1`, or `WR64_WATER_RING_DEBUG=1` to see where it is, painted magenta |
| What it costs | nothing measurable. Over five configurations of the same attract demo, 84 two-second windows each, the port held the game's own 20 frames per second in **98%** of windows with and without it; the full stack -- Best water, the ring, and the draw distance at Extended (then called Maximum) -- held it in 96%. The metric saturates at the game's own rate, so this says the port keeps up, not how much headroom is left |

With both settings at Original nothing is added at all, because a frame with
neither raised has to be the frame the game itself would have produced.

### Environment overrides

For comparing settings without going through the menus. Read once, at the first
frame; they do not persist and they are not settings.

| Variable | Values |
|---|---|
| `WR64_WATER` | `original`, `modern`, `high` |
| `WR64_NO_WATER_RING` | `1` to stop drawing the sea past the game's own patch |
| `WR64_WATER_STYLE` | `classic`, `modern`, `aqua` |
| `WR64_WATER_RIPPLES` | `soft`, `normal`, `strong` |
| `WR64_WATER_SPRAY` | `off` / `0`, anything else is on |
| `WR64_WATER_CLARITY`, `WR64_WATER_BRIGHTNESS`, `WR64_WATER_TINT` | `0`-`100`, as the sliders store them |
| `WR64_WATER_DEBUG` | `0`-`14`, the diagnostic views |
| `WR64_TEST_OPEN_SETTINGS` | `water@25`: opens the settings menu on that tab after that many seconds, for a capture of the menu. Changes nothing saved |
| `WR64_TEST_INSPECTOR` | `25`: presses F1 after that many seconds, opening the HUD inspector and the sun editor for a capture |
| `WR64_WATER_PROFILES` | path to a `profiles.json` to use instead of the packaged one |
| `WR64_WATER=ultra` | a quality above Best that the menu does not offer: the added spray's particle budget doubles |
| `WR64_TEST_WATER_COMPARE_TICK` | a game tick; at it the water flips to Original once, as `F9` does, for a scripted comparison |
| `WR64_WATER_TRACE` | path to write a per-30-frame CSV of craft position and state |
| `WR64_WATER_MATERIAL_TRACE` | set to anything: prints the first few materials handed to the renderer, before and after the style is applied |

`WR64_WATER_MATERIAL_TRACE` is the one to reach for when a setting looks inert.
It distinguishes the two cases that look identical on screen -- the style is
being applied and you cannot see it, versus the material never reaches the
renderer at all -- by printing what was actually sent:

```
[water-trace] quality=2 style=Aqua opticsW=0.0
  deep 0.0080 0.1000 0.1500 a=0.0030 -> 0.0160 0.1850 0.2025 a=0.0006
  shallow 0.0300 0.4400 0.3800 -> 0.0510 0.5720 0.4636  vis 66.7 -> 119.9
  see-through 0.00 -> 1.00  sun -0.450 0.750 0.480 2.00
```

That one is Aqua at Clarity 100%. The `[water] appearance:` line at startup
prints the style and all three sliders, and it prints the values actually in
force: the saved settings, unless an override above replaced one.

No lines at all in a race means the display list carrying the material is not
being emitted, which is a rewriter problem and not a settings one.

`F9` flips between the current setting and Original from the same camera, which
is the only honest way to compare the two. `F10` cycles the diagnostic views.

### What it costs

There is no free version of this. Two measurements, neither of them a benchmark:

| | |
|---|---|
| Apple M3 Max, Metal ([PR #2][pr]) | game-render GPU median/p95 **1.260/1.897 ms → 2.658/4.303 ms**, and a private depth copy of **18.7 MiB** at 1280x956 with 4x MSAA |
| Intel Iris Xe, D3D12 (this tree) | with the renderer engaged and verified on screen, a scripted run through the title, the menus and an attract race holds the game's own 20 fps at **Original**, **Enhanced** and **Best** alike |

The second is still not a frame-cost comparison. The port's frame-rate line
reports the *game's* rate -- the cartridge's 20 or 30 -- which does not move at
all until the machine stops keeping up, so "20 at every setting" means the
headroom was not exhausted here, not that the settings cost the same. A
like-for-like GPU measurement of one camera at both settings has not been made
on this hardware.

The honest summary is the first row: expect the time spent drawing a frame to
roughly double. If your machine is comfortably ahead of the game's rate you will
not see it; if it is not, you will.

### Course profiles

`assets/water/profiles.json` holds one parameter set per course id, staged
beside the executable at build time (inside `Contents/Resources` on macOS).
They are artist-authored art directions, not measurements of the original
lighting. A malformed file is reported and the compiled-in defaults are used, so
a bad edit degrades rather than fails.

**Editing it.** The file is read once, at the first frame: restart the game to
see a change. Edit a copy and point `WR64_WATER_PROFILES` at it to try values
without touching the packaged one. The log says `[water] loaded ten course
profiles`, or `using default profile:` and why. Colours are linear RGB. Values
outside the ranges below are rejected (the four-component fields) or clamped
(the single numbers). The Water tab's sliders apply on top of these, per course.

| Field | Components | Range | What it does |
|---|---|---|---|
| `id` | | 0-9 | the course. 0 Dolphin Park, 1 Sunny Beach, 2 Sunset Bay, 3 Marine Fortress, 4 Drake Lake, 5 Port Blue, 6 Twilight City, 7 Southern Island, 8 Glacier Coast, 9 rider selection. `name` is only a label |
| `deep_color` | RGB, absorption | 0-2; absorption above 0 | body colour of deep water; absorption per game unit, weighted 1.8 / 0.55 / 0.30 across R/G/B so red goes first |
| `shallow_color` | RGB, roughness | 0-2; roughness 0.065 or more | body colour where the wave faces the sun; roughness blurs highlights and reflections |
| `sun_direction` | X, Y, Z, specular | -4 to 4 | **the sun.** XYZ points *from the water toward the sun* in the course's world axes, Y up; it is normalised, so only the direction matters. The fourth number is highlight strength |
| `sun_color` | RGB, ambient | 0-4 | highlight, haze and caustics colour; ambient scales the body, foam and spray |
| `sky_color` | RGB, haze | 0-2 | the sky reflected where the screen has nothing to reflect; haze warms the horizon toward the sun colour |
| `detail` | | 0-2 | ripple strength, times the Surface ripples setting |
| `foam` | | 0-1 | foam amount |
| `wind` | X, Z | -2 to 2 | ripple and foam drift |
| `visibility_range` | | 10-300 | how far into the water the bottom fades out, in units of water depth along the view |
| `authored_reflection` | | 0-1 | **the see-through weight.** Blends the finished water toward the image the game drew before the water, by up to 60% face-on and 100% at grazing angles. One player only. Drake Lake's 0.85 is what keeps its mirrored shoreline; at 1 on any course the floor, fish and the cartridge's own water colour show. Clarity above 50% raises it toward 1 for every course. Not used by Classic |
| `caustics` | | 0-0.5 | light patterns on a shallow floor |

**Aiming the sun at a course's skybox.** Nothing reads the sun from the game;
every course has the value in this file, and six of the ten share the same one,
`[-0.45, 0.75, 0.48, 2.0]`. Y sets the height (a low sun is a small Y, as on
Sunset Bay's `0.23`), and X and Z the bearing on the course's own axes. Do it in
the game rather than here: see [Tuning the sun](#tuning-the-sun).

| Course | `sun_direction` | `authored_reflection` |
|---|---|---|
| Sunset Bay | `[-0.6, 0.23, 0.76, 2.5]` | 0 |
| Drake Lake | `[-0.45, 0.75, 0.48, 0.6]` | **0.85** |
| Twilight City | `[-0.3, 0.15, 0.9, 0.3]` | 0 |
| Southern Island | `[-0.25, 0.86, 0.44, 2.2]` | 0 |
| every other course | `[-0.45, 0.75, 0.48, 2.0]` | 0 |

### Tuning the sun

The water is lit from one direction a course, and on the courses where it was
never matched to the sky the glint sits away from the sun painted there. **Press
F1** during a race: beside RT64's menu and the HUD inspector is **Wave Race
water: sun**, for the course on screen.

```
Sunset Bay (course 2)   * changed
bearing   [----------|----]  -38.3 deg
height    [--|-----------]   13.3 deg
strength  [------|-------]   2.50
[ Aim at camera heading ]  [ Revert to profile ]
[ Save to water_sun.json ]  saved 1 course(s) to water_sun.json
in use   [-0.603, 0.230, 0.764, 2.50]
profile  [-0.600, 0.230, 0.760, 2.50]
```

| Control | What it does |
|---|---|
| **bearing** | Which way the sun is, in degrees on the course's own axes: 0 looks down +Z, 90 down +X. |
| **height** | How high it stands: 0 is the horizon, 90 straight overhead. |
| **strength** | How bright the glint is. Raise it while aiming so the glint is easy to see. |
| **Aim at camera heading** | Sets the bearing to the way the camera faces. Face the sun in the sky and press it. |
| **Revert to profile** | Back to `profiles.json`'s value for this course. |
| **Save to water_sun.json** | Writes every course you changed to the settings folder. The game reads it at startup, so the change stays. |

Changes show on the next frame, and only with Water Quality at Enhanced or Best.

**To ship them**, promote what you saved into the profiles, look at the diff, and
commit:

```
python tools/promote_water_sun.py            # copy the saved suns into profiles.json
python tools/promote_water_sun.py --dry-run  # show what it would change
python tools/promote_water_sun.py --clear    # ... and delete the local file
```

`--clear` is worth doing once promoted: `water_sun.json` overrides the profile,
so while it exists a later change to `profiles.json` does not show on that
machine.

---

## Putting it in another project

What follows is what this is, structurally, for a different N64 port on RT64.

### The shape of it

The game draws its water as an ordinary display list, and RT64 sees only that.
The renderer does not intercept the game's simulation at any point. It:

1. **Takes a snapshot on the game thread**, at the moment the frame's geometry
   and camera are final and nothing has been submitted yet. In this game that is
   `SysMain_SendGfxTaskSetMesg` (USA Rev A `0x80046CF8`), hooked by address in
   `src/overlays.cpp` and wrapped -- not replaced -- in `patches/water.cpp`. The
   snapshot is keyed by the display list pointer at offset `0x30` of the
   `OSTask`, which is what lets the renderer match it on the other thread.
2. **Recognises the water display lists** on the RT64 side -- four of them in
   this game -- and shades that surface instead of drawing it as the game asked.
   Anything it does not recognise falls through to the original path.
3. **Keeps fixed-rate visual fields** for wakes and foam, stepped at 60 Hz
   independently of the presentation rate, so requesting 120 Hz does not change
   which steps run.

The general lesson: **the boundary is the task submission, not the display
list.** A display list tells you what to draw and nothing about what the game
thinks is happening; the moment before it is handed to the OS is where a
consistent view of both exists.

### The material travels in the display list, and something has to put it there

There is no callback from RT64 back into the port for this. The material reaches
the renderer as an **extended GBI command** (`G_EX_WATER_MATERIAL_V1`) written
into the display list immediately before the call that draws the surface, read by
`rt64_gbi_extended.cpp` and attached to the draw that follows; a cleared command
after the call detaches it again. So the port's display-list rewriter has to emit
it, and the material is 1584 bytes travelling in-band once per water draw.

Miss that step and every symptom points the wrong way. The settings save and
reload correctly, the menu behaves, the port's own state is right when you print
it -- and the water is unchanged, because the renderer was never told anything.
It reads exactly like "this setting does nothing".

Two things make it cheap to diagnose: emit the *cleared* command rather than
skipping emission when the feature is off, so "no command at all" unambiguously
means the rewriter is not running; and print the material at the point it is
handed over, which is what `WR64_WATER_MATERIAL_TRACE` does above.

### The problems worth knowing about in advance

**The lattice recenters, so vertex indices are meaningless between frames.**
The game rebuilds its water surface around the camera every frame. Pairing
vertex *n* of this frame with vertex *n* of the last one blends unrelated world
positions, and the waves slide or jerk. The replacement samples the previous
surface at each current world XZ instead. New boundary vertices use current data
only, and camera cuts, course changes and incompatible views reject the history
outright rather than interpolating across them
(`lib/RT64/src/hle/rt64_water_interpolation.h`).

**One-player water tests depth without writing it, and then draws flat horizon
quads after the moving grid.** Under the original blending that is fine. Under
opaque compositing those quads overwrite nearer wave crests. The fix is a
private per-view D32 copy carrying nearest-water depth for the surface and
colour passes, MSAA samples included, leaving scene depth intact for the
original effects that run later. This is where the 18.7 MiB goes.

**The modified transforms are not always there.** The camera for the water pass
comes from `drawData.modViewTransforms[proj.transformsIndex]`, and those are
filled per frame by the projection processor for the workloads in that frame. A
framebuffer added through `State::fullSync` has not been through it, so the
vector is empty and the index reads off the front of it -- a crash on the
graphics thread, in this game on the title screen, the moment the renderer is
switched on. Treat the transforms being present as a precondition for taking
over the draw, not as something to work around: without them this is not a draw
the renderer can shade, and falling through to the game's own water is already
what it does for anything it does not recognise.

**Refraction bleeds at silhouettes.** All four bilinear taps have to be tested
against scene and surface depth; a tap that lands on sky or on foreground
geometry blends toward the water body instead of dragging bright colour through
the edge.

### Reset is something the game must tell you

A restart of the same course changes neither the course id nor the global frame
counter, so a renderer watching only its own inputs cannot tell that the wake
and foam fields should be cleared -- and the previous race's marks carry into
the first frames of the next. Hook the game's race initializer and say so
explicitly. Here that is `func_8009345C`, and the reset happens *after* the
original runs, because the original loads the course.

### Delivery, and why it is a diff

The RT64 side is `tools/patches/rt64-water.patch`, applied by
`tools/patch_rt64_water.py`: a renderer, an interpolation header, a shared
parameter block and ten HLSL shaders, threaded through twenty existing RT64
files. Every other submodule patch in this project is anchored string
replacement, and this one is not, for one reason: at roughly 1200 lines of edits
transcribing it into anchors adds a class of error the diff does not have and
buys nothing. `git apply` already refuses a tree it does not match, which is the
property the anchored scripts exist to provide, and `git apply --reverse
--check` gives idempotency -- it succeeds only when the patch is fully applied,
so a partially applied tree is reported rather than patched again.

It is applied last in `tools/patch_rt64.py`, after the smaller anchored
patches, because it touches several of the same files and applying it first
would make their anchors harder to find.

`tools/patch_runtime_shutdown.py` travels with it and is not optional: without
it the runtime frees its thread queues and RDRAM without waiting for the workers
that may still be reading them, and the process crashes on exit intermittently.
Repeated scripted runs find it; playing does not, for a while.

### A tuning window for per-scene art values

Values like a sun direction are art, not measurement, and the only good test of
one is the picture. This port's editor is three small pieces, and none of them is
specific to water:

1. **An override table beside the loaded asset**, consulted where the per-frame
   material is built (`publish_frame` in `src/water.cpp`): profile value, then
   override. The editor and the game run on different threads -- RT64's UI thread
   and the game's -- so the table is a small array behind one lock.
2. **A window in the renderer's existing debug UI** (`src/watersun.cpp`), drawn
   from the same hook as the HUD inspector (see
   [HUD-INSPECTOR.md](HUD-INSPECTOR.md), *Putting it in another project*). Edit
   in the units the eye works in -- a bearing and a height, not x, y and z -- and
   convert on the way in and out. Take what you can from the game: the camera's
   heading, read on the game thread each frame, turns "match the sky" into one
   button.
3. **Save to the settings folder, promote to the asset.** The save is loaded at
   startup, so a tuned value survives a restart without touching the repository;
   a script copies it into the shipped asset when it is right. Write the asset
   back in its existing formatting, so the diff is the changed numbers.

### Cost control

Make Original the default and make it a real bypass, not a cheaper shading path.
Expose the expensive parts separately -- reflections and spray are Best here,
not Enhanced -- so a player on a slower machine has something between "all of
it" and "none of it". Name the steps as a ladder a player can read without the
tooltip; see *The settings* above for why the names here are not the ids.

---

## Files

| | |
|---|---|
| `tools/patches/rt64-water.patch` | the RT64 side |
| `tools/patch_rt64_water.py` | applies it, idempotently; chained into `patch_rt64.py` |
| `tools/patches/runtime-shutdown.patch`, `tools/patch_runtime_shutdown.py` | the shutdown fix it needs |
| `src/water.cpp`, `include/wr64/water.h` | the port side: settings, the per-frame snapshot, profile loading, the sun overrides |
| `src/watersun.cpp` | the sun editor in the F1 menu |
| `tools/promote_water_sun.py` | copies saved suns from `water_sun.json` into `profiles.json` |
| `include/wr64/water_shore.h` | shoreline segments, from the game's own bounded collision planes |
| `patches/water.cpp` | the two game hooks |
| `src/overlays.cpp` | registers them by address |
| `src/frontend.cpp` | the Water tab and every setting in it |
| `src/callbacks.cpp` | the F9/F10 event watch |
| `src/dlrewrite.cpp` | emits the material into the display list, around the four water draws |
| `assets/water/profiles.json` | per-course parameters |

---

## Keeping this current

This file describes a feature of the *port*. When the renderer changes, when a
measurement here is superseded, or when a setting is added or removed, update it
in the same commit -- and keep the two halves in step, because the second one is
the only reason another project can use any of this.

The design rationale that belongs with the rest of the porting notes is in
[PORTING.md](PORTING.md); facts about Wave Race 64's own water live in
[GAME-INTERNALS.md](GAME-INTERNALS.md).
