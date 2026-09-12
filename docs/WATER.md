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

**High with the Aqua style is the default**, on every platform -- what the fork
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

Everything lives in the **Water** tab, which sits beside Graphics. Its first
setting, **Water**, decides whether the renderer runs at all.

| | What it does |
|---|---|
| **Original** | The game's own water. The renderer is not engaged; nothing below has any effect. |
| **Modern** | Sun and sky lighting, colour that deepens with the water, refraction, persistent wakes behind each craft, wash along the shoreline. |
| **High** *(default)* | Modern, plus screen-space reflections of scenery in view, and fine airborne spray. |

Everything below it shapes how that renderer looks, and none of it has any
effect while **Water** is Original.

| | |
|---|---|
| **Water style** | Listed least to most departure from the cartridge. **Classic** keeps its own colours, transparency, fog and broad highlights and adds only the effects. **Modern** is richer and darker. **Aqua** *(default)* is Modern with a lighter teal and clearer shallows. |
| **Water brightness / Aqua tint / Water clarity** | Aqua only, and hidden under the other two styles. 50% is the default look in each. Clarity is how far you see into the shallows. |
| **Surface ripples** | Soft / **Normal** / Strong. Fine detail added on top of the game's waves, never replacing them. |
| **Spray particles** | The *added* airborne spray. Off keeps the surface foam, the wakes and the game's own splashes. |

### Environment overrides

For comparing settings without going through the menus. Read once, at the first
frame; they do not persist and they are not settings.

| Variable | Values |
|---|---|
| `WR64_WATER` | `original`, `modern`, `high` |
| `WR64_WATER_STYLE` | `classic`, `modern`, `aqua` |
| `WR64_WATER_RIPPLES` | `soft`, `normal`, `strong` |
| `WR64_WATER_SPRAY` | `off` / `0`, anything else is on |
| `WR64_WATER_DEBUG` | `0`-`14`, the diagnostic views |
| `WR64_WATER_PROFILES` | path to a `profiles.json` to use instead of the packaged one |
| `WR64_WATER_TRACE` | path to write a per-30-frame CSV of craft position and state |
| `WR64_WATER_MATERIAL_TRACE` | set to anything: prints the first few materials handed to the renderer, before and after the style is applied |

`WR64_WATER_MATERIAL_TRACE` is the one to reach for when a setting looks inert.
It distinguishes the two cases that look identical on screen -- the style is
being applied and you cannot see it, versus the material never reaches the
renderer at all -- by printing what was actually sent:

```
[water-trace] quality=2 style=Aqua opticsW=0.0
  deep 0.0080 0.1000 0.1500 a=0.0030 -> 0.0160 0.1850 0.2025 a=0.0018
  shallow 0.0300 0.4400 0.3800 -> 0.0510 0.5720 0.4636  vis 66.7 -> 93.3
```

No lines at all in a race means the display list carrying the material is not
being emitted, which is a rewriter problem and not a settings one.

`F9` flips between the current setting and Original from the same camera, which
is the only honest way to compare the two. `F10` cycles the diagnostic views.

### What it costs

There is no free version of this. Two measurements, neither of them a benchmark:

| | |
|---|---|
| Apple M3 Max, Metal ([PR #2][pr]) | game-render GPU median/p95 **1.260/1.897 ms → 2.658/4.303 ms**, and a private depth copy of **18.7 MiB** at 1280x956 with 4x MSAA |
| Intel Iris Xe, D3D12 (this tree) | with the renderer engaged and verified on screen, a scripted run through the title, the menus and an attract race holds the game's own 20 fps at **Original**, **Modern** and **High** alike |

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

### Cost control

Make Original the default and make it a real bypass, not a cheaper shading path.
Expose the expensive parts separately -- reflections and spray are `High` here,
not `Modern` -- so a player on a slower machine has something between "all of
it" and "none of it".

---

## Files

| | |
|---|---|
| `tools/patches/rt64-water.patch` | the RT64 side |
| `tools/patch_rt64_water.py` | applies it, idempotently; chained into `patch_rt64.py` |
| `tools/patches/runtime-shutdown.patch`, `tools/patch_runtime_shutdown.py` | the shutdown fix it needs |
| `src/water.cpp`, `include/wr64/water.h` | the port side: settings, the per-frame snapshot, profile loading |
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
