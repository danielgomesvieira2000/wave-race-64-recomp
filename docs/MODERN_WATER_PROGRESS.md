# Modern water implementation and validation

The native macOS package includes the Modern and High water tiers described in
[the design](MODERN_WATER_PLAN.md). Select **Graphics → Water → Modern or High**
and Apply. **F9** compares against Original while retaining the selected tier;
**F10** cycles diagnostics. Original remains the default. The renderer and
native macOS integration are included in this fork's source build.

## Renderer

| Area | Implemented behavior |
| --- | --- |
| Replacement boundary | Four verified USA Rev A water lists, scoped material metadata independent of interpolation, Original fallback for unknown tasks or invalid cameras |
| Data ownership | Immutable game-thread snapshots tied to graphics tasks, course/race generations, simulation time and four craft identities; no render-thread RDRAM pointers |
| Surface | Original interpolated wave mesh, topology-welded smooth normals, three filtered simplex ripple bands, dielectric Fresnel, rough sun highlights, course sky lighting and fog |
| Shallows | Per-view FP16 pre-water color, resolved scene depth, world-space thickness, absorption, bounded refraction with foreground rejection, optical visibility limits and shallow receiver caustics |
| Reflections | High uses water-only screen-space tracing, a six-level depth hierarchy, confidence rejection, rough filtering and course environment fallback; Drake Lake preserves its original mirrored scene input |
| Interaction | Four world tiles, fixed 60 Hz ripple/foam simulation, distance-based emission, curved wakes, verified wet-contact and landing triggers, advection and decay |
| Shoreline | Original bounded collision planes intersected with current mean water height, twenty nearby segments per craft tile, separate persistent shoreline wash |
| Spray | High adds deterministic particles with gravity, drag, lifetime and soft scene/water intersections; original game spray and effects retain their order |
| Profiles | Ten versioned course/selection presets, validated loading and compiled fallback, authored linear-light values and explicit conversion to the legacy scene target |
| Lifecycle | Pause freezes visual time; race restart, course transitions and discontinuities reset histories; quality changes, resizing and MSAA resources are handled explicitly |

The main frontend is `src/frontend.cpp` wrapping RecompFrontend's RT64 context.
Shared HLSL and the renderer live in RT64, with reproducible project patches.
The original wave field, mesh, buoyancy, collisions and race timing remain
authoritative. Added visual fields never displace the physical water surface.
See [the source audit](MODERN_WATER_SOURCE_AUDIT.md) for height ownership,
draw dependencies, course IDs and shoreline provenance.

## Native visual evidence

The six-second moving review copies are:

- `build/water-qa/final-motion-original/original-moving.mp4`
- `build/water-qa/caustics-on/high-moving.mp4` (final corrected build)

The Original reference predates the field-reset correction, which changes
only the replacement water fields. Both were normalized to 60 fps and fully
decoded. All 49 gameplay checkpoints
match through tick 1410. Every frame-log row in each recording interval has
the expected quality. The camera follows the same deterministic input; native
recorder startup differs by a fraction of a frame, so the videos are not
advertised as frame-exact synchronized captures. Each directory retains
metadata, traces, executable/profile hashes and clean-exit results.
The final High recording contains 417 logged frames with the expected quality
throughout. Its capture directory also contains a same-camera paused
High/Original comparison from the final build.

`build/water-qa/optical-course-{1,2,3,4,7,8}/` covers Sunny Beach, Sunset Bay,
Marine Fortress, Drake Lake, Southern Island and Glacier Coast. These contain
moving High footage and same-camera paused Original/High images with quality
checks around capture. They precede the final shoreline/reset revision; their
hashes are retained rather than treated as current-binary timing evidence.
Port Blue and Twilight City were also refreshed on the final corrected build:
`final-course-5/` and `final-course-6/` contain clean native runs, decoded moving
clips and quality-verified paused comparisons. Their ship/structure reflections
and dark course palette were visually inspected. Dolphin Park and rider
selection are exercised by the normal bring-up replay.

Drake's native scene diagnostic proves mirrored shoreline artwork exists
before water. Its profile preserves that input after fog composition. Sunset
Bay uses a warm reflected horizon. Profiles follow native content: course 7
is Southern Island and 8 is Glacier Coast, reversing the reference header.

`build/water-qa/shore-mask-native/` separates wake red, shoreline wash green,
and crest/depth contact blue in diagnostic 11. Moving inspection places the
green mask along Sunny Beach's edge; GPU statistics independently record its
coverage, mass and peak. Geometry checks cover a known triangle, changing
water height, segment distance and invalid data.

`caustics-on/` and `caustics-off/` change only Sunny Beach's caustic strength
from 0.12 to zero. All 61 gameplay checkpoints match through tick 1770.
At the same paused camera, the sampled shallow edge receives a small positive
brightness change (at most 3/255 per channel); sampled open water, sky and dry
land are identical. This is deliberately subtle, limited by available original
submerged geometry. Animated original pause-menu/craft effects prevent
whole-image equality; `caustics-validation.json` records the controlled regions.

The paused split-screen depth comparison in `build/water-evidence/` identified
an abrupt end to original underwater geometry. Optical visibility now fades
before the exposed diagonal at the same camera. Both player views retain
separate color/depth captures. No seabed is invented where none exists.

## Acceptance results

| Check | Evidence |
| --- | --- |
| Final performance | All 15 Original/Modern/High runs passed at 1080p, 1440p, 2160p, split screen and 120 Hz; every replacement comparison matches 49 gameplay checkpoints; both split-screen water views remain active |
| Final build performance spot check | After the reset correction, another three clean 1080p runs match 49 gameplay checkpoints; `build/water-qa/reset-performance/` |
| Extended temporal matrix | All nine Original/Modern/High 30/60/120 Hz gameplay runs match 97 checkpoints and exit cleanly. After the reset correction, High again passes 97 checkpoints at all three rates, with all 136 ordered wake/shore/particle statistic samples exactly equal; `build/water-qa/reset-boundary/result.json` |
| Pause, camera and same-course restart | `lifecycle-msaa1/` passes 2500 ticks with exit 0. Restart creates generation 5 at tick 2012; its first two whole-second samples have zero wake foam and particles. New shoreline wash comes from current geometry |
| Resize and MSAA | Native zoom changes the target from 1280×876 to 1972×1095 and back; non-MSAA, 2× and 4× paths render; Graphics Apply recreates the target |
| Quality/UI persistence | `graphics-ui-msaa2/` applies Modern then High, F9 shows Original and restores High, F10 cycles back to shaded. `saved-high-relaunch/` reads saved High without an environment override. Prior user settings were restored after testing |
| Shader failure fallback | `final-shader-fallback/` injects initialization failure, retains Original with zero replacement water views in all 298 logged frames, and exits with status 0 |
| Native build | `build/water-full-build-reset-boundary.log` records complete build, package and signature verification |
| Shader backends | `build/water-backends-lifecycle/results.json`: 24 checks pass, twelve variants each to SPIR-V and DXIL; Metal runtime tested, other backends compile-tested only |
| Patch reconstruction | `build/water-patch-reconstruction-reset-boundary.json`: pinned RT64 reconstructs 28 water files exactly, runtime reconstructs four; second application is idempotent |

The final build's 1080p/60 Hz incremental GPU medians are **0.640 ms for
Modern** and **1.109 ms for High** on Apple M3 Max, below the design's 2/4 ms
targets. This timer covers game rendering, excluding the separate VI
presentation/UI queue and display latency. See [the performance
report](MODERN_WATER_PERFORMANCE.md) for resolutions, percentiles, CPU
preparation, memory payload, first-frame costs and measurement limits.

The deterministic fixture covers opening/warm-up, menus and race transitions;
it is not a completed-course or all-weather playthrough. Earlier results before
analog input normalization and clean shutdown enforcement are superseded by
the final matrices. A stop marker alone never counts as a pass: bounded replays
must also exit with status 0.

QA exposed an existing N64ModernRuntime shutdown race: audio/timer workers could
access released RDRAM. The separate runtime patch wakes and joins workers at
scheduler boundaries and joins the timer before memory release, preserving
unrelated runtime modifications.

The extended field comparison also caught a presentation-dependent reset:
initializing a field at the first displayed fractional time skipped different
fixed steps after scene transitions. The renderer now clears at the immutable
game interval boundary and integrates forward, including the first frame.
The failing pre-correction field report remains in `final-physics/fields.json`;
it must not be presented as a passed field comparison.

## Scope and fallbacks

Screen-space reflections cannot supply geometry already culled by the game.
This implementation uses the design's environment/SSR fallback. Drake's
preserved artwork is not a new planar capture. Retained course geometry,
selective planar capture, subdivision, FFT detail and optional bottom proxies
remain follow-on experiments; Ultra is not exposed as a completed tier.

Shoreline coverage is local and budgeted. Original collision data does not
represent every visible coastline; missing/invalid segments add no persistent
wash. Scene-depth contact shading still handles local objects and edges.
Wet land materials and full-scene HDR/tone mapping are outside this water
material. Caustics require actual shallow submerged geometry.

Metal is the runtime acceptance target. Vulkan and D3D12 shaders compile, but
their devices/drivers have not been runtime-tested. Timings apply to this
replay and Mac, with dynamic GPU clocks and a warm system shader cache.
Hardware bandwidth counters were not collected.

## Reproduction

```sh
bash tools/build_macos.sh
python3 tools/run_water_matrix.py --suite performance --output build/water-qa/NEW_PERFORMANCE_RUN
python3 tools/run_water_matrix.py --suite physics --output build/water-qa/NEW_PHYSICS_RUN
python3 tools/run_water_capture.py --course 1 --moving --output build/water-qa/NEW_CAPTURE
python3 tools/verify_water_shaders.py --output build/NEW_BACKEND_CHECK
python3 tools/verify_water_patches.py --output build/NEW_PATCH_CHECK.json
c++ -std=c++17 -Iinclude tools/verify_water_shore.cpp -o build/water-shore-check
build/water-shore-check
```

The app is `build-macos/WaveRace64Recomp.app`. Native capture helpers come from
the installed screenshot and record-gui-tutorial skills. Run matrices
sequentially against one unchanged bundle, without recording or diagnostic
GPU field/pass instrumentation during performance measurement.

`tools/run_water_replay.py` owns a bounded child and records settings/hashes.
It supports process-local quality, rate, resolution, MSAA, course, players,
profiles and shader-failure overrides. `--stats` enables exact fixed-step GPU
field readback; `--pass-profile` adds pass queries. `--saved-quality` verifies
UI persistence. These overrides do not save graphics preferences.

| Environment variable | Purpose |
| --- | --- |
| `WR64_WATER=original\|modern\|high` | Process-local initial quality |
| `WR64_WATER_DEBUG=0..14` | Shaded, mask, normals, world grid, source color, thickness, scene color, valid depth, SSR confidence, reprojection error, craft contact, foam, wireframe, field atlas, field coordinates |
| `WR64_WATER_PROFILES=/absolute/profiles.json` | Alternate versioned profiles |
| `WR64_WATER_TRACE=/absolute/state.csv` | Ordered game-state checkpoints |
| `WR64_WATER_STATS=/absolute/fields.csv` | Foam/shore coverage, mass, peaks, particle count and contact inputs |
| `WR64_WATER_PROFILE=/absolute/passes.csv` | Optional GPU pass queries |
| `WR64_INPUT_SCRIPT=/absolute/fixture.ticks` | Game-update playback; `@ticks` isolates hardware input |

Use absolute paths because the app changes working directory. Launch without
test/input overrides for human play; exclusive QA input is not a playable
handoff.

The final handoff launches the signed bundle executable directly with the ROM
path, without replay, quality or timing environment overrides. High was selected
and applied through the native graphics UI, then inspected in the live attract
sequence. `build/water-qa/acceptance-summary.json` indexes final evidence and
records the playable process. A LaunchServices launch with the ROM argument
stalled in the initial file open on this macOS installation; that owned process
was stopped and the normal direct executable launch succeeded. No OS privacy
settings were changed.

## Water appearance controls and foam correction

Graphics retains water quality; the Water tab controls appearance. Water style offers Modern
(the existing shading) and Classic (the original course texture, transparency,
fog and broad wave highlights, with added reflections, glints and foam). Surface
ripples offers Soft, Normal and Strong; Normal retains the previous detail
strength. These settings affect presentation only. Both styles keep the original
wave mesh, timing and gameplay simulation. F9 still compares against the complete
Original renderer.

Classic first draws the original water at its original place in the render order,
copies its color to a separate view-scoped texture, and layers modern effects over
that base. The MSAA path retains each coverage sample in a two-column atlas and
uses a sample-frequency Classic shader, avoiding a second application of edge
coverage. New lighting fades with course fog; original artwork is not fogged or
tone-compressed a second time. Appearance preferences snapshot once per display
list so an Apply operation cannot change resource requirements halfway through a
view. Classic adds an original raster pass, a color copy and sample shading, so
its cost differs from Modern even at the same Water quality.

The old foam breakup multiplied two axis-aligned sine waves, producing repeating
square holes. It now uses warped, rotated noise with distance filtering. The
persistent wake field and spray simulation are unchanged.

For reproduction, both QA runners accept `--style modern|classic` and
`--ripples soft|normal|strong`. Replay `--saved-appearance` checks saved menu
preferences. Equivalent process-local overrides are `WR64_WATER_STYLE` and
`WR64_WATER_RIPPLES`; they do not save preferences.

A shutdown race exposed during appearance validation was also repaired. RT64's
Present worker could wait on an already-destroyed WorkloadQueue mutex. Both queues
now receive cancellation and wake their ID/interpolation waiters while both
objects remain alive; only after joining all workers are the queues released.

Appearance acceptance is indexed in `build/water-qa/style-acceptance.json`.
Final bundle `392677c272663490f03780367241a4d84c929f1c66cffde80c6d041e910363a2`
passed the full Sunny Beach Classic/Strong capture and Port Blue two-player
Classic normal diagnostic at 2x MSAA, both with clean exits. The same shader
source was reviewed in Modern/Normal and Classic/Soft at foggy Drake Lake with
1x MSAA. The 49 shared gameplay checkpoints match between Modern and Classic.
All 32 SPIR-V/DXIL shader checks and 33-file RT64 / 4-file runtime patch
reconstruction checks passed. Native menu Apply and persistence were verified
after relaunch; High quality with Classic/Strong was left selected in the normal
playable app, without QA or input overrides.

## Smooth wave interpolation

The original water lattice recenters around the camera in roughly 64-unit steps.
Its stable vertex count had been mistaken for stable world correspondence. RT64's
index-based interpolation therefore translated the entire grid and blended UVs
from different wave slopes, particularly visible in Classic's broad highlights.

Modern and Classic now sample the previous water surface at each current world
XZ. Exact shared points retain their previous data; other covered points sample
the actual previous triangles, including the original integer coordinate rounding.
Only heights and reflection UVs interpolate, so recentering cannot drag the waves
sideways. New perimeter points use their current state. Course/generation changes,
camera cuts, incompatible projections and player viewports reject old history.
This changes presentation only; the game's authoritative simulation and the
existing fixed-step wake/spray simulation retain their timing. Original quality's
untagged raster path is unchanged.

The CPU check exercises grid shifts, rounded row coordinates, ordinary deformation,
duplicated RSP vertices, uncovered boundaries, resets and view separation:

```sh
c++ -std=c++17 -O2 -Wall -Wextra -Werror tools/tests/water_interpolation_test.cpp -o build/water-interpolation-check
build/water-interpolation-check
```

Replay `--motion-trace` writes optional CPU vertex/UV and interpolation-weight
evidence. `tools/summarize_water_motion.py BEFORE AFTER --course 1
--require-spatial-match --output REPORT.json` removes verified dry setup records
and reconciles the remaining records with the renderer frame log. Its timestamps
measure render preparation, not displayed FPS. Captures and gameplay traces are
separate checks. `run_water_capture.py --fps 120` requests a 120 Hz replay/capture;
source recordings can drop frames and the normalized review copy does not prove
display cadence. Benchmark summaries reject motion-instrumented runs.

The native spatial checks pass for Classic and Modern at 120 Hz, Classic at
60/30 Hz, and both Port Blue player views at 120 Hz. Across 222 common positive
Sunny Beach intervals, all 102 previous recenter jumps disappear while retaining
vertical and UV animation. All 49 gameplay checkpoints match across rates/styles
and the prior two-player build. All 68 GPU wake/shore/spray field samples match
exactly between 30 and 60 Hz. CPU tests pass with strict warnings and ASan/UBSan;
35-file RT64 and 4-file runtime patch reconstruction is byte-exact and idempotent.
The reviewed native motion capture retains Classic's color, waves and wake detail.
These checks establish corrected interpolation, not a guarantee of locked display
FPS; actual workload timing and capture frame loss are recorded separately.

Acceptance is indexed in `build/water-qa/smooth-acceptance.json` for bundle
`22c75de8d5da7158347b937ac7124ff0ba6959b8eaf5dd9c210689e26f021e07`.
The repeat uninstrumented Classic/Strong 1080p, 4x MSAA, 120 Hz run produced
1,196 of 1,206 target renderer frames in submissions 1190–1390 (99.17%). GPU
workload median/p95/max were 4.18/5.92/7.76 ms. The earlier uninstrumented run
had substantial CPU/GPU spikes and is retained in the same index; the figures
do not establish a locked presentation rate. The signed, tested bundle was
relaunched without QA/input overrides, with High, Classic and Strong preserved.

## Spray control and near-camera flashes

Water → Spray particles offers Off/On and saves with the other Water preferences.
On preserves the default added spray; Off suppresses its draw immediately in both
Modern and Classic styles, including while paused. Surface foam, wakes, original
game splashes, and the fixed-step particle state remain unchanged. Keeping state
advancing makes re-enabling the effect continuous and avoids restarting the wake
simulation. Spray is an added High-quality effect; Original game spray is separate.

The spray vertex shader previously divided the particle center and upper edge by
their clip W before checking visibility. An edge approaching or crossing the eye
plane could generate an unbounded billboard. It now initializes finite offscreen
geometry, rejects invalid/behind-eye projections before division, fades nearby and
oversized particles, and caps projected radius. This removes that concrete hazard;
it does not prove that every reported white flash came from the spray pass.

A preferences vector is appended to WaterMaterial. Display-list payload version 2
transfers 1,584 bytes; version 1 remains readable as its original 1,568 bytes and
defaults spray to On. The shared draw-parameter stride is 1,904 bytes, below the
D3D structured-buffer limit. Preferences snapshot once per display list.
Both QA runners accept `--spray off|on`; `WR64_WATER_SPRAY=off|on` is a process-local
override. Replay `--saved-appearance` leaves the saved spray preference active too.

Spray acceptance is indexed in `build/water-qa/spray-acceptance.json` for bundle
`ba4699e29286a820c2d1ef3abe6711fb16cbc8b4da154079c46d14da1fc27a04`. The native Water menu was used to select and Apply Off;
saved preferences and the subsequent normal-launch log confirm it persists.
All 32 shader compilation and 35-file RT64 / 4-file runtime patch checks pass.
Classic On and Modern Off both complete their native 120 Hz replay and retain the
spatial wave-matching checks. Their actual race agrees at all eight state
checkpoints and eleven fixed-step field samples. The recorded Modern Off run
has thirteen initial-attract state differences (ticks 180–540) and twenty initial
field differences; these are retained in the evidence, with cause undetermined.
Every state checkpoint from tick 570 onward agrees. The earlier full on/off
comparison is not reported as passing. The normal playable app is left running
with the saved spray setting Off; original splashes and surface wakes remain.

## Irregular ripple shading

The fine ripple normal previously summed six coherent cosine plane waves. Its two
strongest directions crossed at approximately 90 degrees, producing a resolved
hatching pattern rather than only a sampling artifact. Strong amplified the same
pattern. The welded, area-weighted mesh normals did not explain those multiple
thin stripes inside a triangle and remain unchanged.

The detail now sums three independently rotated simplex-gradient bands, stretched
across the wind and advected at different speeds. Random local gradients replace
continuous crossed stripes. Analytic derivatives include domain rotation and
stretch; band heights scale inversely with frequency so fine bands cannot dominate
slope energy. A conservative two-dimensional pixel footprint fades each scale
before it becomes subpixel. The gain of 0.04623 matches the prior unfiltered RMS
slope of about 0.1205 at Normal/detail 1; Soft/Normal/Strong keep their scale factors.
Broad waves, original Classic shading, wakes, optics and gameplay are unchanged.

`build/ripple-preview/calibration.json` records 1,572,864 float32 samples across
six wind/time/world-range conditions. Its synthetic planar image is a math/shading
preview, not native runtime acceptance. The QA runners accept an optional `--app`
path for testing a separate signed bundle without replacing an active game.

Native acceptance is indexed in `build/water-qa/ripple-acceptance.json` for bundle
`b9e305bb1d68c269c14538a1fdbf3c1ae028d09e8bbf9b66964ae5087ffd1a83`. Before/after
Modern Strong captures show the repeated thin bands replaced by irregular detail.
All 49 state checkpoints and 1,422 input rows match exactly, and both 60 Hz runs
produce all 603 expected race render records. The candidate's measured GPU median
is 1.957 ms versus 1.681 ms before at 1280x876 with 4x MSAA. This is one sequential
capture pair, not an isolated benchmark. All 32 shader and 35/4-file patch checks
pass; an independent analytic derivative review found no continuity defect.

A separate 120 Hz Championship run completes without shader/runtime failure,
but produces 1,097 of 1,206 target render records in submissions 1260-1460. Its GPU
median/p95/max are 1.746/3.361/13.099 ms at an actual 1920x986 extent. It is not a
locked-120-FPS result, nor a physics comparison against the different Time Trials
fixture. GPU timing excludes VI/UI presentation. Native videos have variable
capture timestamps; normalized copies do not establish displayed frame rate.

The exact tested signed bundle is installed at `build-macos/WaveRace64Recomp.app`,
the temporary CMake output override is removed, and normal launch is verified in
the native UI. Current user preferences remain Modern / Strong / Spray Off,
High quality, manual 60 Hz and 4x MSAA.

## Water self-occlusion and refraction edges

The original one-player water tests scene depth without writing it and submits
its six flat horizon quads after the animated grid. Modern water composites
refraction into an opaque result, so inheriting that state allowed the distant
skirt to overwrite nearer wave crests. Modern replacements of this translucent
path now copy the original D32 attachment, including all MSAA samples, into
a private depth target once per view/render. The existing surface pass writes
nearest water depth there; the color pass tests LESS_EQUAL without writing.
That also gives spray the nearest surface distance. Later original effects keep
the unmodified scene depth. Classic and originally opaque two-player water keep
their existing depth behavior. Pipeline keys distinguish Classic shader choice
from Modern self-depth testing, and view keys include the depth policy.

Refraction now checks all four bilinear color taps against surface/scene depth.
Foreground and clear-sky taps contribute no transmitted color; their fractional
coverage blends to the water body. Previously a valid center depth could still
blend a neighboring bright sky/foreground color through a water edge. This is
a concrete sampling hazard; the user reported that intermittent light-blue
patches were no longer visible before this candidate was installed, so that
observation is not attributed to the new sampling change.

The repeatable capture runner accepts `--mode`, `--script`, and `--moving-at`
to examine later race sections. A custom start tick requires `--motion-only`
and extends the owned replay long enough for the six-second capture.

Acceptance is indexed in `build/water-qa/horizon-acceptance.json` for bundle
`11766750efc2f08004078f9dc6b7e6797f58c3d424b23b8731cb371f5fa02db7`.
The native Dolphin pair agrees at 49/49 gameplay checkpoints and the later Sunny
pair at 61/61. Moving captures show no obvious new seam or hole, but do not
recreate the exact large-crest camera in the user's screenshot. Normal installed
launch was also checked in the native UI with larger Dolphin Park intro waves.
All 32 shader checks and 35/4-file patch reconstruction checks pass. Metal API
Validation was explicitly enabled on a separate single-sample Modern run, which
exited cleanly; a Classic two-player 2x-MSAA run also exited cleanly. Those two
concurrent runs differ from their physics references, so only the sequential
capture pairs are reported as exact gameplay acceptance.

The private 1280x956 4x-MSAA D32 target adds about 18.7 MiB in the measured
Dolphin case. Recorded GPU median/p95 increased from 1.260/1.897 to 2.658/4.303 ms
in that one capture pair; both have all 603 expected race render records. This is
rendering-workload evidence, not an isolated benchmark or display-FPS guarantee.
The exact signed/tested bundle was installed, the temporary CMake output override
removed, and the normal app reopened with Modern / Strong / Spray Off preserved.

## Optional Aqua appearance

The Water tab now offers Aqua alongside Modern and Classic. Aqua retains the
Modern render path and changes only the per-draw optical material: brighter
teal body colors, reduced absorption and a longer but bounded underwater
visibility fade. Course palette and lighting remain the basis, including
sunset and night courses. Wave geometry, interpolation, ripple detail,
roughness, reflections, caustics, foam, wakes and spray are unchanged.

The style enum remains backward compatible (Modern 0, Classic 1, Aqua 2).
The renderer's classic-base flag is still boolean: only Classic sets it.
Material adjustments operate on a copy, so repeated draws and split-screen
views cannot accumulate brightness or visibility changes. Aqua is now the
default for fresh settings, with existing explicit style choices preserved.
Selecting Aqua reveals brightness, tint, and clarity sliders; 50% on each
restores the reviewed preset. Their values are captured once per display list
and applied only to Aqua, with bounded clarity to retain underwater edge fades.
`WR64_WATER_STYLE=aqua` and the replay/capture tools' `--style aqua`
option allow process-local comparisons.

The macOS build and seven configuration/haptics regression checks pass. A
2,001-tick native High/Aqua replay at a 120 Hz presentation target completed
with a clean exit; the Sunny Beach water was visually inspected in that run.
