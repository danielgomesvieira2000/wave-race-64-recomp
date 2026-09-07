# Modern water rendering plan

Prepared from this working tree and the running native macOS build on 2026-09-06. This document preserves the original design. See [implementation and validation](MODERN_WATER_PROGRESS.md) and [measured performance](MODERN_WATER_PERFORMANCE.md) for the subsequently built Modern and High tiers.

The recommended direction is a dedicated modern water renderer inside the existing RT64 raster pipeline. Preserve the original large waves and their timing, then add physically based surface lighting, fine ripples, depth-dependent color, reflections, persistent wakes, and spray. Aim for the game's vivid seaside appearance with much richer light and motion. Apple Silicon/Metal is the first implementation target, with shared HLSL and scalable quality for the other supported backends.

The running start-line scene has broad, soft cyan highlights and little fine surface detail. That observation is a single view, not an audit of every course or an in-motion comparison. The first implementation milestone must capture repeatable moving comparisons.

**What the current code provides**

| Finding | Evidence and implication |
| --- | --- |
| The normal frontend build wraps RecompFrontend's renderer. | `src/frontend.cpp:67–116`: `RewritingContext::send_dl` rewrites the display list before forwarding it. Connect the feature here and in RT64; changes confined to the older `src/renderer.cpp` context will not implement it for the normal app. |
| Water is already identified separately. | `src/dlrewrite.cpp:614–691` identifies perspective display lists whose first vertex source is segment 3; `1424–1430` brackets them with `kWaterId`. This is an interpolation classification, not a custom material API. |
| The original mesh is animated on the CPU. | The rewriter's recorded investigation describes 50 vertex blocks, with stable indexing in the sampled race frames. Vertex positions and texture coordinates are already interpolated. Verify this correspondence across courses, cameras, and both players before generalizing it. |
| The reference includes useful water logic, but much is assembly. | `reference/wr64-decomp/src/game/water_69D0.c` contains the water routines and accesses `D_80162420`; `gWaterLevel` and `Game_801CE608.waveLevel` are named elsewhere. These are investigation entry points, not a verified public height-query API. |
| Water drawing changes with course and player count. | `reference/wr64-decomp/src/game/code_43DA0.c:312–396`, `Draw_WaterEffects`, uses different lists, fog, formats, and blending for Drake Lake and split screen. Do not assume one material state or list works everywhere. |
| RT64 raster input is designed for N64 rendering. | `lib/RT64/src/shaders/RasterVS.hlsl` receives processed screen positions, UVs, and colors; `RasterPS.hlsl` implements N64 combining/blending. A water material needs additional positions, normals, camera data, and textures. |
| Interpolation data exists, but has conditions. | `RSPProcessCS.hlsl:63–66` interpolates source positions before projection. `rt64_workload_queue.cpp:390–401` only runs world-vertex processing for a matched previous frame, and `rt64_framebuffer_renderer.cpp:324–334` binds existing world resources in a ray-tracing-specific block. Make water resources available on ordinary raster frames, including first frames and camera cuts. |
| The existing UI render hook is too late. | `rt64_present_queue.cpp:346–353` invokes it after the VI presentation pass. Add a scene-level extension at `FramebufferRenderer::submitRasterScene`, not a swapchain overlay. |
| Cross-platform shader compilation is available. | `lib/RT64/CMakeLists.txt:105–181` compiles HLSL to DXIL/SPIR-V and, on macOS, SPIR-V through MSL into a metallib. Use this route for the new vertex, pixel, and compute shaders. |
| Metal hardware ray tracing is not implemented in this pinned backend. | `lib/RT64/src/contrib/plume/plume_metal.cpp:3818–3819` and its ray-tracing creation methods. The baseline must use raster and compute. |
| RT64's `usesHDR` name does not establish a floating-point lighting pipeline. | `rt64_render_target.cpp:508–513` selects 16-bit UNORM or 8-bit UNORM color, plus D32 depth. Audit transfer functions and use explicit floating-point intermediate targets if needed; merely enabling this flag does not provide unconstrained HDR lighting. |

**The intended appearance**

| Feature | Visible result | Initial implementation |
| --- | --- | --- |
| Fine surface structure | Ripples break up broad highlights; the water responds to camera angle and wind. | Smooth geometric normals plus two or three world-aligned normal/derivative layers at different scales, directions, and speeds. Filter aggressively at distance. |
| Surface lighting | Sun glitter, soft rough reflections, darker troughs, and brighter wave faces. | Dielectric Fresnel, a rough microfacet sun highlight, sky lighting, and a restrained wave-thickness scattering approximation. |
| Depth and refraction | Clearer shallows, richer deep color, submerged hull distortion, and natural shoreline transitions. | A water-excluded scene color/depth copy, bounded distortion, absorption, and approximate in-scattering. |
| Reflections | Scenery and riders connect visually to the water; horizon reflections remain stable. | A course sky/environment fallback first, then screen-space reflections. Add selective planar capture for suitable courses after scene capture is proven. |
| Wakes and foam | Each craft leaves a turning trail that spreads, breaks up, and fades instead of remaining attached to a sprite. | Persistent world-space wake/foam fields, driven by stable craft identities and motion; crest and shoreline foam have separate masks. |
| Spray | Acceleration, hard turns, and landings feel forceful. | Event-driven spray particles with velocity, gravity, drag, soft depth intersections, and light response. |
| Course identity | Water belongs to the setting rather than using one global blue material. | Per-course absorption, wind, roughness, foam, sky/sun, and fog settings; author presets after comparison captures. |

Use Sunny Beach for clear shallows and warm highlights; Drake Lake for restrained ripples, fog, and calm reflections; Marine Fortress for structure reflections and turbulent wakes; Sunset Bay for warm grazing light. These are proposed art directions, not claims that the original game exposes corresponding modern lighting parameters. Later cover Southern Island, Twilight City, Glacier Coast, every remaining course, and their alternate race conditions.

**Architecture and data ownership**

The proposed flow is:

```text
Original game update and display lists
    -> water identification and immutable frame metadata
    -> RT64 workload, with view-scoped water draw ranges
    -> RT64 interpolation at presentation time
    -> water geometry/normals + scene inputs
    -> modern water shading and interaction effects
    -> remaining effects, game overlays, and frontend UI
```

1. Make water identification independent of `water_interp`. The current `draws_water` predicate returns false when interpolation is disabled; turning off interpolation must not silently disable the modern material. Keep `kWaterId` for temporal matching and introduce explicit material metadata or a scoped extended command for shading. Never treat that transform ID as an existing shader hook.
2. Identify water conservatively using course/game state, view, list identity, source segment, and topology. Reset list-classification caches when segment mappings or course generations change. Validate the current heuristic before suppressing any original draw. Unknown contexts retain the original material.
3. Pass metadata through the actual RecompFrontend context into the RT64 workload. Add a small optional extension interface; keep Wave Race course rules in the main project. Material state must split draw batches and reset at its end so adjacent geometry cannot inherit it.
4. Capture game-side values at a synchronized task/update boundary. Associate copied data with a simulation tick, workload/frame generation, course, and view. Hold it until GPU completion; do not retain live pointers into RDRAM or read game globals asynchronously from the render thread.
5. Keep simulation time distinct from presentation time. The game chooses 20/30 Hz depending on mode; RT64 renders intermediate frames. Evaluate fine visual waves at the same interpolated time as the mesh and camera. Consume each wake/splash event once. Fixed-step visual fields can interpolate their results for display. Reset histories at cuts, resets, teleports, course changes, and renderer recreation; freeze appropriate effects on pause.
6. Keep the original wave simulation authoritative for buoyancy, collisions, launches, and race timing. Initially shade the existing interpolated mesh. Later subdivide its triangles or sample a verified original height field to improve geometric detail. Preserve original anchors and cap any added displacement near craft, buoys, ramps, and shorelines. A large independent ocean simulation would require a separate gameplay integration project to keep craft contact consistent.
7. Provide explicit world positions and geometric normals for the water shader. Either export them during RSP processing or use a dedicated water compute/vertex route with RT64's exact adjusted transforms and interpolation weights. Validate reprojection against the original mesh. Original vertex colors must not be reinterpreted as geometric normals. Generate normals using topology and weld appropriate block boundaries to avoid seams.
8. Namespace camera matrices, masks, render targets, and temporal history by framebuffer and player view. Share world interaction data where appropriate; do not share one player's reflection history or screen-space coordinates with the other.

**Milestone 1 — Instrument and establish the replacement boundary**

Deliver a debug build that colors only the water and reports its draw ranges, triangle count, vertex correspondence, view identity, material state, interpolation weight, and GPU time. Record wireframe, geometric normals, source colors, and water coverage as separate views.

Capture a straight run, a hard turn, a jump/landing, and a camera change. Repeat in one-player championship, Time Trial, warm-up, and split screen. Establish whether the water mesh and the physics height query agree, rather than inferring that from function names. Record the normal scene draw order and all framebuffer reads/writes around water.

The existing `WR64_3D_TRACE`, `WR64_PAIRING`, and `WR64_NO_WATER_INTERP` switches are useful. The input script in `src/testdrive.cpp` runs on elapsed wall time and the sample race script contains old bring-up timings; it is not a deterministic replay test. Add input playback indexed by game update if exact comparisons are required.

Acceptance: the mask covers every water variant in the selected scenes and no riders, scenery, HUD, or spray. Switching the debug material off restores the original image. First frames and scene cuts work without relying on a previous matched frame.

**Milestone 2 — Ship the first visible material upgrade**

Implement dedicated `WaterVS.hlsl` and `WaterPS.hlsl` on the existing mesh, with reconstructed smooth normals, filtered detail normals, Fresnel, rough sun highlights, and course sky/environment lighting. Begin with approximate water-body color while scene depth is unavailable. Avoid adding original baked highlights on top of new specular lighting.

Use a defined linear-light working space for the water calculations and a documented conversion at the legacy scene boundary. Calibrate against the original course colors and fog. Keep high-intensity water values in a local FP16 target if required, then map them consistently into the existing scene. Full-scene exposure, display HDR, and a new tone mapper are separate follow-on work.

Expose `Original` and `Modern` in the graphics UI, plus a comparison control usable from the same camera position. Shader changes must be reproducible through the existing build scripts. Handle disable/re-enable, resizing, shader failure, and graphics-context recreation.

Acceptance: the rebuilt native app visibly improves moving water on Sunny Beach, with no topology cracks, crawling texture coordinates, unstable glitter, incorrect fog, or change in craft motion. This is the first reviewable deliverable and can stand alone while the deeper renderer work continues.

**Milestone 3 — Add scene depth, absorption, and refraction**

Build a water-specific scene pass with explicit resources. The desired dependency order within a validated view is:

```text
Scene sky and relevant opaque/alpha-tested geometry
    -> immutable scene color + scene depth, excluding water
    -> water surface depth/mask and normals
    -> water refraction, absorption, lighting, and reflections
    -> spray and other correctly ordered transparent effects
    -> game overlays and HUD
```

This is a target dependency graph, not an assumption that the N64 list is already organized this way. Preserve framebuffer operations and order-dependent blending. Where opaque content arrives after the original water draw, gather a safe draw subset or perform a dedicated scene capture from copied render data. Do not reorder every triangle globally. Keep uncertain cases on the original path until their dependencies are understood.

Resolve/copy color and depth into separate readable resources with correct barriers; never sample an actively written attachment through an unsupported feedback path. Preserve the opaque depth before water writes its own depth. Reconstruct view/world depth with the actual projection and N64 depth conventions; subtracting raw depth-buffer values does not yield water thickness. Design MSAA depth resolution and edge coverage explicitly.

Use bounded screen-space refraction with foreground rejection so the rider or a pier above the water cannot be pulled into an underwater sample. Derive optical path length from valid background geometry, cap it at a course-controlled maximum, and fall back to deep-water color when no background exists. A water shader cannot reveal seabed geometry absent from the game; author optional shallow-bottom proxies only where inspection shows they are needed.

Use depth for local contact transitions, but use stable course masks or distance fields for persistent shoreline foam and wet edges. Shoreline data should account for water-level changes. Add caustics only on valid shallow submerged surfaces; suppress them beneath deep water, opaque foam, and unlit areas.

Acceptance: shorelines, piers, hulls, and ramps have correct depth relationships during motion, including viewport edges, close camera angles, and split screen. No duplicate water draw, reflected HUD, ghosted transparency, or excessive visibility through missing terrain.

**Milestone 4 — Improve reflections without sacrificing stability**

Start with roughness-filtered course environment/sky reflections. Add water-only screen-space reflection tracing using water normals and a scene depth hierarchy, with thickness checks, confidence weighting, and a fallback where data is unavailable. Temporal accumulation needs correct water motion and rejection of newly exposed pixels; begin without accumulation if reliable motion data is not yet available.

For calm water and prominent nearby scenery, add an optional planar reflection of selected geometry around the course's mean water level. Use a reflected camera, appropriate clipping/culling, a reduced-resolution target, roughness filtering, and displacement-aware distortion. It approximates a wavy surface and must blend away where that approximation is poor.

A crucial investigation is off-camera geometry. Replaying only the main view's submitted triangles cannot reflect scenery the game already culled. Build a validated retained course-geometry set or a dedicated render-only capture path from immutable state. Dynamic craft need similarly valid geometry. Never run gameplay again to produce a reflection. If coverage cannot be supplied safely, retain the environment/SSR fallback rather than promise complete planar reflections.

Acceptance: reflections remain plausible when scenery leaves the screen, through camera pans and cuts. Reflections contain neither water recursion nor UI. Record both GPU and CPU cost; smaller reflection textures do not remove geometry submission cost.

**Milestone 5 — Add persistent interaction and higher detail**

Create a world-space wake field with stable craft IDs, previous/current position, heading, speed, and verified contact/landing events. Start with trails stamped by traveled distance so rendering at 120 Hz does not emit twice as much foam as 60 Hz. Build a V-shaped wake, a turbulent center trail, and turn-dependent spray. Advect, spread, and decay foam over time; reset discontinuities instead of drawing long lines across teleports.

Use a compact fixed-step visual ripple/foam simulation in tiled course space or a scrolling field that preserves and reprojects retained contents. Account for all craft and both players. Avoid a purely camera-attached effect whose trail moves with the viewer. Preserve or selectively replace original spray only after corresponding effects have been identified, so splashes are neither doubled nor lost.

Generate spray from verified motion/events, with reproducible seeds and a particle budget. Depth-soften particles against water and scenery and use the same sun/fog parameters as the surface. Contact foam, breaking-crest foam, and long-lived wake foam need separate triggers and lifetimes; a single scrolling white texture will not deliver the intended result.

Once shading and contacts are stable, optionally add a denser mesh through compute-generated subdivision/LOD with crack-free boundaries. Reuse the original height data and add only controlled detail. A multi-band FFT spectrum is a later quality experiment for fine detail or distant open water; it is not a dependency for the first release. Fade extra geometric detail by distance and contact constraints, with filtered normal detail taking over beyond mesh resolution.

Acceptance: wakes persist in the world, follow curved paths, respond to landings, and have comparable density/lifetime at 30, 60, and 120 Hz. Craft remain attached to the visible surface. Pause, restart, split screen, and course transitions leave no stale trails.

**Milestone 6 — Tune courses and package a scalable release**

| Preset | Intended contents | Initial incremental GPU target at 1080p, one player |
| --- | --- | --- |
| Original | Current pipeline. | Baseline. |
| Modern | Existing wave mesh, rich normals, sun/sky lighting, depth color, bounded refraction, modest wakes/foam. | At most about 2 ms. |
| High | Modern plus screen-space reflections, richer spray, and higher-resolution interaction fields. | At most about 4 ms. |
| Ultra | Selective planar reflections, denser geometry, expanded particles, and optional shallow caustics. | At most about 6 ms; hardware-dependent. |

These are proposed budgets to validate on the target Mac, not measured performance or frame-rate promises. Total frame budget is 16.67 ms for 60 Hz and 8.33 ms for 120 Hz, including the existing game and renderer. Measure median and slow-frame times, CPU submission, memory/bandwidth, and first-use shader stalls at 1080p, 1440p, and 4K. Re-evaluate budgets for split screen. Scale reflection resolution, trace count, field resolution, and particles before reducing core surface quality.

Verify feature support and compile variants on Metal, Vulkan, and D3D12 as hardware is available. Cross-compilation alone is not runtime validation. Keep the shared implementation within verified raster/compute capabilities; avoid making hardware tessellation or ray tracing a baseline requirement.

Tune water alongside the course sky, haze, and sun direction: these are reflected in most of the screen. Preserve readable buoys, ramps, riders, and HUD. More realistic water will expose the simplicity of the surrounding assets, so a later coordinated sky/shoreline/lighting pass offers a clearer next graphics improvement than indiscriminate post-processing.

**Proposed implementation locations**

| Location | Responsibility |
| --- | --- |
| `src/dlrewrite.cpp` | Harden water identification and emit scoped material metadata independently of interpolation. |
| `src/frontend.cpp`, RecompFrontend renderer adapter | Connect water configuration and immutable frame data to the actual application renderer. |
| New `include/wr64/water.h`, `src/water.cpp` | Course profiles, event collection, frame snapshots, generation IDs, and feature lifecycle. |
| RT64 draw-call/workload structures and `rt64_framebuffer_renderer.cpp` | Carry metadata, expose raster water resources, preserve draw dependencies, execute the custom passes. |
| New RT64 water renderer module and HLSL shader family | Surface shading, scene copies, depth reconstruction, reflection tracing, interaction simulation, and spray. |
| New `assets/water/` | Authored normal/foam maps and versioned course profiles; assets need a reproducible source and license. |
| `CMakeLists.txt`, RT64 shader build, project patch tooling | Register new shaders and reproduce library changes. Follow the repository's scripted patch approach or use an explicitly pinned fork; do not rely on unrecorded submodule edits. |
| `src/testdrive.cpp` and new render diagnostics | Tick-indexed input, comparison captures, geometry/contact checks, and per-pass GPU timings. |

The exact extension API should be settled in milestone 1. The current submodules and macOS integration already have local changes; implementation should record that baseline and isolate new work without overwriting it. Generated `RecompiledFuncs` and reference assembly are evidence, not primary edit targets.

**Release evidence**

Require a launched, rebuilt native app and same-camera before/after clips for every milestone. Use deterministic game-update input and selected gameplay-state checks to verify that the visual feature preserves race timing and craft behavior. Compare contacts, shoreline edges, fog, sun highlights, reflection failure cases, and wake persistence at normal speed and frame by frame. Test first frames, pause, resets, camera cuts, graphics settings, resizing, and both split-screen views. Advance through the course matrix before enabling Modern by default.

The key unknowns are the original height-query semantics, complete water/list coverage, capture of off-camera scene geometry, absent underwater geometry, and the legacy scene's color/depth conventions. Milestones 1 and 3 explicitly resolve these. The shader math is only part of the work; renderer integration and temporal/visual validation determine whether the result feels native to this game.

**Technical references used for the design**

- [NVIDIA GPU Gems: Effective Water Simulation from Physical Models](https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models) describes separating geometric undulation from finer normal-map waves. The proposal applies that separation while keeping this game's original large-wave geometry.
- [Epic: Single Layer Water Shading Model](https://dev.epicgames.com/documentation/en-us/unreal-engine/single-layer-water-shading-model-in-unreal-engine) documents a dedicated water pass using scene color/depth, absorption, scattering, and reflection composition before regular translucency. It is a design reference; those resources and passes still need to be implemented in this RT64 integration.
- [Epic: Planar Reflections](https://dev.epicgames.com/documentation/en-us/unreal-engine/planar-reflections-in-unreal-engine) explains the screen-edge/off-screen limitations of SSR and the extra scene-rendering cost of planar capture. The proposed retained-geometry requirement follows from combining that technique with this game's submission and culling behavior.
