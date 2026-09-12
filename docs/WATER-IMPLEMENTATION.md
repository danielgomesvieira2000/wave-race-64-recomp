# The modern water renderer: implementation, and how this repository differs

What the fork actually built, component by component, and what this repository
took, changed, dropped and added. Written for someone who has to modify it, or
build the same thing somewhere else.

[WATER.md](WATER.md) is the shorter document: what the settings do and a recipe
for another port. This one is the full account, including the parts that only
matter when you are inside the code.

## Provenance

| | |
|---|---|
| Source | [PR #2](https://github.com/danielgomesvieira2000/wave-race-64-recomp/pull/2), `elliotttate:main`, closed |
| Fork base | `8b77185`, 65 commits behind this repository's `main` at the time of the merge |
| Water commits | `272ee0b` (initial), `c4659b2` (Aqua defaults and sliders) |
| Submodule pins | **identical** to this repository: RT64 `5473732`, N64ModernRuntime `cdf5abb`, RecompFrontend `b1a1477` |
| Target it was built on | Apple M3 Max, Metal, macOS 27.0 |
| Backends the fork validated at runtime | Metal only. Vulkan and D3D12 compiled but were never run. |

Identical submodule pins are why any of this transplants at all: the RT64 diff
applies to the same revision this repository builds.

---

# Part 1 — the fork's implementation

## Data flow

```
game thread                          graphics thread
-----------                          ---------------
SysMain_SendGfxTaskSetMesg
  └─ water::publish_frame(rdram, list)
       snapshot: time, course, generation,
       4 craft, 20 shore segments/tile
       → pendingFrames, keyed by display list
                                     dlrewrite::rewrite(list)
                                       └─ water::begin_frame(list)
                                            claim snapshot by key
                                       └─ around each water draw:
                                            G_EX_WATER_MATERIAL_V1 + payload
                                            <the game's own draw call>
                                            G_EX_WATER_MATERIAL_V1 + 0
                                                        │
                                     RT64 GBI_EXTENDED::waterMaterialV1
                                       → State::waterMaterial
                                       → DrawCall::waterMaterial
                                                        │
                                     FramebufferRenderer::addFramebuffer
                                       → WaterRenderer passes
```

Nothing is read from RDRAM on the graphics thread, and no pointer into RDRAM
outlives the snapshot.

## The boundary: what is read from the game, and where

| Address (USA Rev A) | Symbol | Role |
|---|---|---|
| `0x80046CF8` | `SysMain_SendGfxTaskSetMesg` | wrapped; the frame's geometry and camera are final and nothing is submitted yet |
| `0x8009345C` | `func_8009345C` | wrapped; the complete race initializer, used to reset visual history |
| `0x80047E44` | `SysUtils_Srand` | wrapped; **test fixture only**, forces a deterministic seed |

Both real hooks wrap rather than replace: the original is called from inside.
Registration is by address through `recomp::overlays::add_loaded_function` in
`src/overlays.cpp`, not by weak-symbol override, because the address is what the
runtime dispatches on.

The display list a task will run is the 32-bit pointer at offset `0x30` of the
`OSTask`. That value, masked to `0x7FFFFF`, is the key that matches a game-thread
snapshot to a graphics-thread rewrite.

Read out of RDRAM per frame, in `water::publish_frame`:

| | Address | Meaning |
|---|---|---|
| tick | `0x80151960` | global frame counter, drives simulation time |
| state | `0x800DAB24` | game state, for the trace only |
| course | via profile lookup | selects one of ten profiles; `> 9` disables the renderer for the frame |
| craft | `0x80192690` + `n * 0x1718` | four craft: world XYZ and wet-contact fraction |
| shoreline | game collision planes | intersected with mean water height, 20 segments per craft tile |

`include/wr64/water_shore.h` does the shoreline intersection in doubles: it
takes the game's own bounded collision plane (the 16 floats `func_8007F448`
uses), intersects it with the mean water plane, and clips against the same four
half-spaces. Near-horizontal beaches make the division ill-conditioned, which is
why it is not done in floats.

## The material ABI

`lib/RT64/src/shared/rt64_water_params.h`. Every field is a `float4` so the
display-list, C++, SPIR-V and MSL layouts are identical — there is no packing to
disagree about.

| Field | Contents |
|---|---|
| `deepColor` | linear RGB, absorption scale per game unit |
| `shallowColor` | linear RGB, roughness |
| `sunDirection` | direction toward sun, intensity |
| `sunColor` | linear RGB, ambient intensity |
| `skyColor` | linear RGB, horizon haze |
| `animation` | previous/current simulation seconds, detail strength, foam strength |
| `identity` | **quality (0 disables)**, debug view, course, generation |
| `surface` | mean height, wind X/Z, view index |
| `optics` | visibility range, authored reflection weight, shallow caustics, classic style |
| `craftPrevious[4]`, `craftCurrent[4]` | world XYZ, wet-contact fraction (`-1` inactive) |
| `shoreCounts` | bounded shoreline segments per craft tile |
| `shoreSegments[80]` | XZ endpoints, `WATER_SHORE_SEGMENTS_PER_TILE` = 20 |
| `effects` | payload v2: added spray enabled |

Sizes are asserted, not assumed:

```
static_assert(sizeof(WaterMaterial)    == 1584);  // display-list ABI
static_assert(sizeof(WaterDrawParams)  == 1904);  // shader ABI
static_assert(sizeof(WaterDrawParams)  <= 2048);  // D3D structured-buffer stride
static_assert(sizeof(WaterFieldPushConstants) == 96);
```

`identity.x` is the master switch and it travels *in the material*: quality `0`
means the renderer does nothing for this draw. That is why Original costs
nothing beyond the command itself.

## Transport: an extended GBI command, not a callback

There is no callback from RT64 into the port. `G_EX_WATER_MATERIAL_V1`
(`0x000034`, raising `G_EX_MAX` to `0x000035`) carries the payload inline in the
display list.

`GBI_EXTENDED::waterMaterialV1` reads `w1` as a version, then copies
`payloadBytes / 8` command words straight into the struct:

| Version | Payload | Behaviour |
|---|---|---|
| 1 | up to `offsetof(WaterMaterial, effects)` | `effects.x` forced to 1 (spray on) |
| 2 | `sizeof(WaterMaterial)` | full struct |

The copy happens immediately, in the handler, "before the game reuses its
display-list memory". The result lands in `State::waterMaterial` and is attached
to the following draw through `DrawAttribute::WaterMaterial` (28) —
so it splits draw batches and a cleared command detaches it again.

## RT64: new files

| File | Lines | Role |
|---|---:|---|
| `src/render/rt64_water_renderer.cpp` | 752 | passes, resources, fields, spray |
| `src/render/rt64_water_renderer.h` | 212 | descriptor sets, state |
| `src/hle/rt64_water_interpolation.h` | 183 | previous-surface sampling by world XZ |
| `src/shared/rt64_water_params.h` | 68 | the ABI above |
| 11 × `src/shaders/Water*.hlsl` | ~700 | below |

Eight descriptor sets: `WaterDescriptorSet` for the shading pass, plus
`WaterStatsSet`, `WaterSpraySet`, `WaterFieldSet`, `WaterDepthReduceSet`,
`WaterCaptureSet`, `WaterOriginalSet` and `WaterNormalSet`.

## RT64: shaders

11 sources compile to 16 variants, each to both DXIL and SPIR-V, and on macOS
through SPIR-V → MSL into a metallib.

| Shader | Lines | Variants | Role |
|---|---:|---|---|
| `WaterPS.hlsl` | 407 | base, `CLASSIC_STYLE`, `CLASSIC_STYLE+CLASSIC_MSAA` | all surface shading |
| `WaterFieldCS.hlsl` | 88 | — | fixed 60 Hz wake and foam fields |
| `WaterSprayPS/VS.hlsl` | 42 / 39 | PS also `SPRAY_MSAA` | airborne spray billboards |
| `WaterCaptureCS.hlsl` | 32 | also `CAPTURE_MSAA` | pre-water colour and depth copy |
| `WaterOriginalCS.hlsl` | 27 | also `CAPTURE_MSAA` | F9 Original comparison capture |
| `WaterNormalsCS.hlsl` | 22 | — | topology-welded smooth normals |
| `WaterStatsCS.hlsl` | 22 | — | diagnostic readback |
| `WaterDepthReduceCS.hlsl` | 20 | — | six-level depth hierarchy for SSR |
| `WaterVS.hlsl` | 18 | — | surface vertex |
| `WaterSurfacePS.hlsl` | 9 | — | water-only depth prepass |

`CMakeLists.txt:162` special-cases `/Water` in the shader build rule.

## RT64: the twenty edited files

| File | Hunks | What it contributes |
|---|---:|---|
| `hle/rt64_workload_queue.cpp` | 14 | the bulk: water resource lifecycle, per-view state, field stepping, reset on discontinuity |
| `hle/rt64_game_frame.cpp` | 4 | water draw ranges per view, interpolation wiring |
| `render/rt64_framebuffer_renderer.cpp` | 4 | **where the passes run**, inside `addFramebuffer` |
| `hle/rt64_present_queue.cpp` | 4 | presentation-time interaction with the water targets |
| `hle/rt64_state.cpp` | 3 | `waterMaterial` state, attribute change tracking |
| `hle/rt64_draw_call.h` | 3 | `DrawAttribute::WaterMaterial = 28`, `DrawCall::waterMaterial` |
| `gbi/rt64_gbi_extended.cpp` | 3 | the command handler above |
| `CMakeLists.txt` | 8 | shader variants, new sources |
| `render/rt64_rsp_processor.cpp`, `shaders/RSPProcessCS.hlsl` | 2 | world positions exported on ordinary raster frames, not only for ray tracing |
| `hle/rt64_workload.{cpp,h}`, `rt64_state.h`, `rt64_present_queue.h`, `rt64_workload_queue.h`, `render/rt64_descriptor_sets.h`, `render/rt64_framebuffer_renderer.{h,_call.h}`, `include/rt64_extended_gbi.h` | 1–3 each | declarations, plumbing |

## The per-frame pipeline

Inside `FramebufferRenderer::addFramebuffer`, per identified water range:

1. **Capture** pre-water scene colour (FP16) and resolved depth — `WaterCaptureCS`, MSAA variant where needed.
2. **Depth reduce** to a six-level hierarchy — `WaterDepthReduceCS`, for screen-space reflection tracing.
3. **Normals** from the original mesh topology, welded across block boundaries — `WaterNormalsCS`.
4. **Fields** stepped at fixed 60 Hz — `WaterFieldCS`, four world tiles, distance-based emission, curved wakes, advection and decay.
5. **Surface depth prepass** — `WaterSurfacePS`, giving the water its own nearest-depth copy.
6. **Shade** — `WaterVS` + `WaterPS`: Fresnel, rough sun highlight, sky lighting, absorption, bounded refraction, SSR on High, shoreline wash, foam.
7. **Spray** on High — `WaterSprayVS/PS`, with gravity, drag, lifetime and soft scene/water intersection.

## Three problems the design exists to solve

**The lattice recenters.** The game rebuilds its water around the camera in
roughly 64-unit steps. Vertex *n* is not the same point between frames, so
index-paired interpolation blends unrelated world positions and the waves slide.
`rt64_water_interpolation.h` keeps the previous surface in a bounded BVH of
triangles (`sampleTriangle`, `sampleNode`, `sample(x, z)`) and samples it at each
current world XZ, interpolating height and reflection UVs. New boundary vertices
use current data only. `compatible(cur, prev)` rejects history across camera
cuts, course changes and incompatible views.

**One-player water tests depth without writing it.** `Draw_WaterEffects` uses
`AA_ZB_XLU_SURF`, then draws six flat skirt quads *after* the moving grid. Under
the original blending that is fine; under opaque compositing a later skirt
fragment paints over a nearer wave crest. The fix is a private per-view D32 copy
carrying nearest-water depth for the surface and colour passes, MSAA samples
included, leaving scene depth intact for the original effects that run after.
Classic and the originally opaque two-player water keep their existing policy.

**Refraction bleeds at silhouettes.** All four bilinear taps are tested against
scene and surface depth; a tap landing on sky or foreground blends toward the
water body instead of dragging bright colour through the edge.

## Course profiles

`assets/water/profiles.json`, schema version 1, exactly ten courses, validated
on load: unknown or duplicate ids, non-finite values and out-of-range components
all reject the file and fall back to compiled defaults with a diagnostic. Values
are linear RGB, artist-authored, explicitly *not* measurements of the original
lighting.

## The runtime shutdown patch

Not optional, and not about water except that repeated scripted water runs are
what found it. `tools/patches/runtime-shutdown.patch` touches five files across
librecomp and ultramodern:

| File | Change |
|---|---|
| `ultramodern/src/threads.cpp` | `cleanup_shutdown_requested`, wake and join the workers |
| `ultramodern/src/timer.cpp` | `join_timer_thread()` |
| `ultramodern/include/ultramodern/ultramodern.hpp` | declares it |
| `librecomp/src/recomp.cpp` | calls it before releasing queues and RDRAM |

Without it the runtime frees thread queues and RDRAM while workers may still be
inside a wait on them. Intermittent, and only reliably visible under repeated
automated runs.

## The fork's tooling

| Tool | Lines | Purpose |
|---|---:|---|
| `summarize_water_motion.py` | 221 | motion comparison between captures |
| `run_water_replay.py` | 150 | deterministic replay against checkpoints |
| `run_water_capture.py` | 147 | recorded capture runs |
| `run_water_matrix.py` | 108 | resolution/rate matrix |
| `summarize_water_benchmarks.py` | 94 | GPU/CPU timing tables |
| `verify_water_shaders.py` | 69 | 32 checks: 16 variants × DXIL and SPIR-V |
| `verify_water_patches.py` | 60 | reconstructs 35 RT64 + 4 runtime files from pinned revisions |
| `compare_water_traces.py` | 43 | trace diffing |
| `water_interpolation_test.cpp` | 124 | CPU test: recentering, rounded grid coordinates, duplicate vertices, boundaries, resets, separate views |
| `verify_water_shore.cpp` | 31 | shoreline intersection |
| `scripts/water/*.ticks` | 14–21 | tick-exact input fixtures |

## Measured performance, Apple M3 Max / Metal

Final build, 1080p/60 Hz, 1920×986 target, 4× MSAA, 603 moving frames per tier,
each replacement matching all 49 gameplay checkpoints.

| Quality | GPU median | GPU p95 | CPU prep median | Water payload |
|---|---:|---:|---:|---:|
| Original | 0.970 ms | 1.420 ms | 0.201 ms | 0 MiB |
| Modern | 1.610 ms | 2.708 ms | 0.475 ms | 62.1 MiB |
| High | 2.078 ms | 3.089 ms | 0.492 ms | 74.1 MiB |

| Output / rate | Original | Modern | High | High p95 | High delta |
|---|---:|---:|---:|---:|---:|
| 1080p / 60 | 0.963 | 1.576 | 2.017 | 2.993 | +1.054 ms |
| 1440p / 60 | 1.467 | 2.089 | 2.440 | 2.959 | +0.973 ms |
| 2160p / 60 | 1.689 | 2.934 | 4.616 | 5.056 | +2.927 ms |
| 1080p / 60 split | 0.917 | 1.906 | 2.632 | 3.416 | +1.716 ms |
| 1080p / 120 | 1.134 | 1.326 | 1.695 | 2.924 | +0.562 ms |

First replacement frame, 1080p/60: Modern 6.299 ms CPU / 3.101 ms GPU; High
6.464 / 5.211. Warm-cache limitation, not a steady-state figure.

The PR body quotes a different pair (1.260/1.897 → 2.658/4.303 ms) from an
earlier capture workload. The table above is the fork's own final measurement
and is the one to use.

---

# Part 2 — this repository

## Taken unchanged

| | |
|---|---|
| `tools/patches/rt64-water.patch` | byte-identical, 3178 lines |
| `tools/patches/runtime-shutdown.patch` | byte-identical, 186 lines |
| `include/wr64/water.h`, `include/wr64/water_shore.h` | identical |
| `assets/water/profiles.json`, `assets/water/README.md` | identical |
| `src/water.cpp` | identical but for the two changes below |

The RT64 diff applies to this tree with hunk offsets of 1–23 lines and no
conflicts, despite this repository's own RT64 patches touching four of the same
files (`rt64_framebuffer_renderer.cpp`, `rt64_game_frame.cpp`, `rt64_state.cpp`,
`rt64_application.cpp`). It is applied **last** in `tools/patch_rt64.py`, after
the smaller anchored patches, so their anchors are found in unmodified text.

## Changed

| Component | Fork | This repository | Why |
|---|---|---|---|
| Delivery | `patch_water.py`, 36 lines, no idempotency check | `patch_rt64_water.py`, `git apply --reverse --check` plus a marker | a second run must be a no-op, and a partially applied tree must be reported rather than patched again |
| Patch ordering | water before `patch_texture_packs.py` | water last in the `patch_rt64.py` chain | it touches files this repository's own patches anchor into |
| Enum ids | lower-case (`"high"`, `"aqua"`) | **same** — adopted after a mismatch | ids are what land in the settings file; an id matching nothing resolves silently to another entry |
| Settings layout | `water_quality` in Graphics, the rest in a Water tab | all six in the Water tab, beside Graphics | a tab whose every entry depends on a setting in *another* tab reads as a tab that does nothing. Moving it also moves where it is stored, from `graphics.json` to `water.json`, so a value saved by the fork's layout is not carried over |
| `material()` | as written | + `WR64_WATER_MATERIAL_TRACE` | see *the bug* below |
| `patches/water.cpp` | 59 lines | 46 lines | test fixtures and texture-capture removed |
| Default quality | `High` | `High` | initially set to `Original` here for frame cost; changed back on request |
| `addFramebuffer` water precondition | none | `waterTransformsReady` | see *the crash* below |

## Dropped

| | Why |
|---|---|
| `water_test_seed_hook` (`0x80047E44`) | `WR64_TEST_SEED` fixture, for the fork's deterministic replay harness |
| `WR64_TEST_COURSE`, `WR64_TEST_PLAYERS`, `WR64_TEST_MODE` in the race-init hook | same harness; they write the game's mode and course globals directly |
| `texture_capture_before_race` / `_after_race` calls | the fork's HD texture pipeline, which this repository does through the mod system |
| `haptics::capture_frame`, `haptics::reset_for_race` | this repository grew its own `src/haptics.cpp` in the 65 commits since the fork branched |
| `music_*` hooks | likewise `src/music.cpp` |
| Aqua sliders' `WR64_TEST_*` paths, the replay/capture/matrix tooling | depends on the fork's QA harness and its `build/water-qa/` outputs, which are not in the PR |

`patches/water.cpp` went from 59 lines to 46 as a result, and the two remaining
hooks are the two that matter.

## Added

| | |
|---|---|
| `WR64_WATER_MATERIAL_TRACE` | prints the material at the hand-off point, before and after the style transform |
| `waterTransformsReady` guard | the precondition the renderer assumed |
| `docs/WATER.md`, this file | the fork's four design documents are a working record; these are the distilled reference |
| F9/F10 removal on shutdown | `SDL_DelEventWatch` in `shutdown_platform()` |

## The bug: nothing reached the renderer

The fork's `src/dlrewrite.cpp` changes are 54 lines. This repository's
`dlrewrite.cpp` had moved 834 lines since that branch, and the water wiring was
not carried across in the first pass. The result:

- every setting saved and reloaded correctly
- the menu behaved
- the port's own state was right when printed
- `water::material()` was **never called once**, in any configuration
- the water was unchanged, which reads exactly as "this setting does nothing"

Because the material travels in the display list, the rewriter *is* the
transport. Missing it is silent.

What was added to `dlrewrite.cpp`:

| | |
|---|---|
| `water::begin_frame(list_vaddr)` | at the top of `rewrite()`, claims the snapshot for this list |
| `known_water_material(list)` | the four lists `0x010082F0`, `0x0100B590`, `0x0100D258`, `0x0100E680` |
| `water_material(bool)` | emits `G_EX_WATER_MATERIAL_V1`, payload v2, or the cleared command |
| `draws_water()` | no longer gated on `water_interp` |
| call site | material emitted around the draw, interpolation group gated separately |

`draws_water()` answers "is this the water surface" well enough to hang an
interpolation group off; `known_water_material()` is the stronger claim needed
before handing a list to the renderer, since a wrong answer would shade craft
spray as though it were the sea.

Emitting the *cleared* command rather than skipping emission is deliberate: "no
command at all" then means the rewriter is not running, which is a different
fault from "the feature is off".

## The crash: a precondition the renderer assumed

With the material finally arriving, Modern and High both died on the title
screen — `ACCESS_VIOLATION` reading `0x44` in
`FramebufferRenderer::addFramebuffer`, on the graphics thread, under
`State::fullSync`. Original was clean.

The water pass takes its camera from
`drawData.modViewTransforms[proj.transformsIndex]`. Those are filled by
`ProjectionProcessor::process`, which copies the raw transforms for the
workloads in the frame it is handed. A framebuffer reaching `addFramebuffer`
through `fullSync` has not been through that, so the vector is empty and the
index reads off the front of it.

```c++
const bool waterTransformsReady =
    proj.transformsIndex < drawData.modViewTransforms.size() &&
    proj.transformsIndex < drawData.modViewProjTransforms.size();
```

folded into `wantsWater`. It is a precondition, not a workaround: with no
transforms this is not a draw that can be shaded, and falling through to the
game's own water is what the renderer already does for any list it does not
recognise.

The guard is anchored in `patch_rt64_water.py` after the diff, so the symptom
stays next to its explanation. Because it edits a line the diff introduces, it
breaks `git apply --reverse --check` as an idempotency test — the guard's marker
is therefore the authority on "fully applied".

## Verification: what was checked where

| | Fork (M3 Max, Metal) | This repository (Iris Xe, D3D12) |
|---|---|---|
| Shader compilation | 32 checks, DXIL + SPIR-V + Metal | 16 variants to DXIL and SPIR-V, in the normal build |
| Patch chain from pristine | `verify_water_patches.py`, 39 files reconstructed | full chain re-applied to clean submodules, then rebuilt |
| Runs without crashing | yes, with Metal API Validation | yes, title → menus → attract race, Original / Modern / High |
| Water visibly renders | video and paused comparisons, 8 courses | screenshot comparison at Sunset Bay, Original vs High + Aqua |
| Material values | — | `WR64_WATER_MATERIAL_TRACE`, Modern vs Aqua |
| Gameplay equivalence | 49/49 and 61/61 checkpoints, 1422 input rows | not attempted — no replay harness here |
| GPU frame cost | full matrix, five configurations | **not measured** |
| Split screen | 2-player, 2× MSAA, clean exit | not attempted |
| Vulkan | compiled only | compiled only |
| Linux, macOS | macOS yes | **neither has run this** |

The frame-rate line this port prints reports the *game's* rate — the cartridge's
20 or 30 — which does not move until the machine stops keeping up. "20 fps at
every setting" on the Iris Xe says the headroom was not exhausted, not that the
settings cost the same.

## Known gaps

| | |
|---|---|
| No GPU frame-cost measurement on this hardware | the fork's Apple numbers are the only real before/after |
| No gameplay-equivalence harness | the fork's replay tooling depends on QA outputs not in the PR |
| Vulkan and D3D12 never runtime-accepted by the fork | D3D12 now has a run here; Vulkan still has none |
| Split screen untested here | the fork measured it; this repository has not run it |
| `Quality::Ultra` exists in the enum | not offered in the menus, in either tree |

---

## Keeping this current

This file is a comparison against a fixed point: PR #2 as of `9041cc8`. When the
renderer changes here, update *Part 2* and leave Part 1 alone — it describes what
the fork built, which does not change. If the RT64 diff is ever re-derived or the
submodules move, say so in *Provenance*, because everything in Part 1 is pinned
to those revisions.

[WATER.md](WATER.md) is the user-facing half and the port-it-elsewhere recipe;
[PORTING.md](PORTING.md) holds the general lessons that are not specific to
water; [GAME-INTERNALS.md](GAME-INTERNALS.md) holds the facts about Wave Race 64
itself.
