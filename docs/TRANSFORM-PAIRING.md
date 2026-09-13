# Transform pairing: interpolation that pairs the right cameras, riders and objects

RT64 draws the frames between the game's by pairing what it sees in one frame
with the previous frame's -- **cameras** first, then the **objects** drawn under
each camera -- and moving everything between the two. When it pairs the wrong
two, the picture is drawn from somewhere it never was.

Three defects here, and the fix for each -- the first two inside RT64
(`tools/patch_rt64.py`), the third in the port's display-list rewriter
(`src/dlrewrite.cpp`):

| Defect | What was wrong | Fix |
|---|---|---|
| **The 2P race-start burst.** For over a second after a two-player race fades in, each view flickers between two shots. | For 33 consecutive game frames each view's camera was paired with *another* view's previous camera. | A camera only continues a previous camera on the same framebuffer slot that drew into mostly the same part of the screen. |
| **Riders coming apart.** In a race, one part of a rider -- an arm, a leg, a piece of the craft -- jumps ahead of the body every few frames; on the watercraft select screen, parts jump when the selection changes. | RT64 pairs a rider's parts by draw signature and nearest position. Mirrored parts share a signature, so a part's own previous pose is taken by its twin, or found under a different draw call when the mesh changes. The part left without a pair is drawn at its new place, then snaps again the next frame because it restarts from zero velocity. | Each part is paired **by identity**: an explicit transform id from its matrix's address, set by the rewriter before every part's matrix load, with its translation always interpolated. |
| **Objects gliding across the course.** Purple course-edge buoys sliding along their rows, plainest in the opening sequence at maximum draw distance; repeated objects -- buoys, markers, spray -- paired with one hundreds to thousands of units away and drawn sliding between them. | RT64 accepts any object pair, whatever the distance. | A pair further apart than 150 world units is refused. |

And the instruments that found them: a **pairing log** that records every
camera and object pair, a **window capture** that records every presented
frame even with something on top of the game, and **input scripts** for a
two-player race and for switching riders.

Two halves: [**using it**](#using-it) and [**putting it in another
project**](#putting-it-in-another-project). The mechanism and the wrong turns
are in [PORTING.md](PORTING.md) §7, *Interpolation*; the game facts in
[GAME-INTERNALS.md](GAME-INTERNALS.md) §6.

---

## Using it

### The switches

All three fixes are on in every build.

| Environment variable | Effect |
|---|---|
| `WR64_NO_SCENE_REGIONS=1` | Cameras are paired by matrix alone again, as upstream RT64 does. The 2P burst comes back. |
| `WR64_NO_MODEL_IDS=1` | Riders' parts are paired by RT64's heuristic again. Parts come apart in races and on the select screen. |
| `WR64_PAIRING_MAX_JUMP=<units>` | The object pair limit. Built in at **150**; `0` switches it off. |
| `WR64_PAIRING_LOG=<file>` | Write the pairing log. Nothing is written without it. |
| `WR64_PAIRING=1` | The two-second counter of unpaired transforms, beside the frame rate. |

The check worth running before anything else: present at the game's own frame
rate, so that no frames are interpolated. Anything that survives is not
interpolation; anything that disappears is nothing else.

### Reproducing a two-player race start

```
WR64_INPUT_SCRIPT=tools/scripts/race-2p.txt
```

drives both players from boot into a 2P VS race on Sunny Beach -- no second pad
needed. Buttons prefixed `2:` in a script are player two's, and a script that
uses the prefix makes the port report a second controller connected. Timings
on this laptop, seconds after launch:

| | |
|---|---|
| Race state entered (`TIME_TRIAL`, mode `2P_VS`) | ~32 |
| Fade-in | ~34.2 |
| Close-up intro, both views | 34.5 - 36.5 |
| Cut to the start line, countdown | ~36.5 - 39.5 |
| Go | ~39.5 |
| **The burst, before the fix** | **from ~35.0, for 33 game frames (~1.6 s at the race's 20 a second)** |

### Reproducing a rider switch

```
WR64_INPUT_SCRIPT=tools/scripts/rider-select.txt
```

walks to the watercraft select screen (state `RIDER_SELECT`, ~19.5 s) and
moves the selection right four times and left four times, 1.5 s apart, from
24 s. Two of the eight switches land on a rider whose model has 23 parts
rather than 18.

### Capturing what is on screen

```
python tools/capture_frames.py shots 33 42 --env WR64_INPUT_SCRIPT=tools/scripts/race-2p.txt
python tools/contact_sheet.py lower.jpg 4 0.5 --dir shots --from 34950 --to 35480 --crop 0 270 960 540
```

`capture_frames.py` launches the port and saves every frame its window presents
between two times, as `tMMMMMM.jpg` named by milliseconds since launch, at
30-40 a second at half size -- every other frame at 60 Hz. It reads the
window's own contents through Windows Graphics Capture, so **a terminal on top
of the game does not end up in the picture**, which it does with
`capture_window.ps1`. Windows only; `pip install windows-capture opencv-python
pillow`. `--env NAME=VALUE` sets a variable for that run only. It captures the
window **owned by the process it launched**: looked up by title alone, a copy of
the game someone is already playing is found first and recorded instead, which
happened here and produced twenty seconds of the wrong screen.

`contact_sheet.py` tiles a span of frames into one image. `--crop` is what makes
a split-screen defect readable: on a 960x540 capture the bottom view is
`0 270 960 540`.

What the burst looks like, bottom view, consecutive captures:

| Before | After |
|---|---|
| near shot of rider 95, far shot, near, far with rider 2 in the foreground, near... | one smooth dolly toward rider 95 |

### The pairing log

`WR64_PAIRING_LOG` names a file. For every game frame RT64 matched:

```
F 760 wall=1789276803875
V persp cur=2/4 fb=1 n=1 scissor=32,488,1244,916 prev=2/4 fb=1 n=1 scissor=32,488,1244,916 diff=305.14
T 3 3 auto id=FFFFFFFF call=A19C9E2B0213B85E range=3-3 cur=4400.0,0.0,-2000.0 prev=4400.0,0.0,-2000.0 jump=0.0 lerp=1 pvel=0.0
U 0 id=FFFFFFFF call=4C55F4BEBF41AB80 range=0-0 cur=0.0,0.0,0.0
S frame=760 total=93 paired=92 j50=0 j100=0 j200=0 max=15.8 refused=8
```

| Line | Meaning |
|---|---|
| `F n wall=ms` | A matched game frame. `wall` is milliseconds since the Unix epoch; `capture_frames.py` writes the launch time in the same units to `launch.txt`, so frame `F` is near capture `t(wall - launch)`. |
| `V` | One **camera** pair, as RT64's `matchScenes` decided it. `cur=i/n`: scene *i* of this frame's *n*. `fb`: the framebuffer slot of the scene's first projection. `n`: projections in the scene. `scissor`: the union of its projections' scissors, **quarter pixels** (`32,488,1244,916` is (8,122)-(311,229)); `2147483647,...` is a scene that drew nothing. `diff`: RT64's matrix difference between the two cameras. |
| `T t p path` | One **object** pair: transform *t* with previous transform *p*, `path` `id` (explicit id, linear ordering) or `auto` (call-hash heuristic). |
| `call=`, `range=` | RT64's hash of the draw call and the matrix indices it spans. Same hash = candidates for each other. |
| `cur=`, `prev=`, `jump=` | The two translations and the distance between them, world units. |
| `lerp=` | Whether the translation is interpolated this frame (RT64's velocity check). |
| `pvel=` | The previous frame's velocity for that object. Large `pvel` on a small jump = the wake of an earlier wrong pair. |
| `U t` | No pair: drawn at the current matrix. Harmless for an object on its own; **a part of a moving model** drawn here is a frame's motion away from the parts that glide, and on the next frame it restarts from zero velocity and snaps again. |
| `S` | Frame summary; `refused` = object candidates the jump limit turned away. |

A two-player race frame is ~300 lines; a two-minute run 12-20 MB.

### Reading it

```
python tools/pairing_log.py pairing.log --scenes        # camera pairs across framebuffer slots or screen regions
python tools/pairing_log.py pairing.log --models        # moving models (riders) with a part unpaired or snapped
python tools/pairing_log.py pairing.log --over 150      # object pairs further apart than 150
python tools/pairing_log.py pairing.log --calls         # object pairs grouped by draw call
python tools/pairing_log.py pairing.log --frames 700-900
```

`--scenes` on the same 2P start, before and after:

```
891 frames, 2873 scene pairs; 99 paired across framebuffer slots or screen regions; 0 frames with a scene left unpaired
  frame 747 persp: fb 1 (8,122)-(311,229)  <-  fb 0 nothing drawn  (difference 199.2)
  frame 747 persp: fb 0 nothing drawn  <-  fb 1 (8,12)-(311,120)  (difference 216.8)
  frame 747 persp: fb 1 (8,12)-(311,120)  <-  fb 1 (8,122)-(311,229)  (difference 699.7)

893 frames, 2880 scene pairs; 0 paired across framebuffer slots or screen regions; 0 frames with a scene left unpaired
```

A scene left unpaired is normal when the previous frame had fewer: a view that
has just appeared has nothing to continue. Two scenes with the same camera and
one inset inside the other (difference 0) overlap by the rule and are not
reported.

### What the numbers were

**Cameras, 2P VS start, Sunny Beach:**

| | before | after |
|---|---:|---:|
| Frames with views paired across regions | **33 consecutive** (747-779) | **0** |
| Pattern | bottom <- empty scene on slot 0, empty <- top, top <- bottom | each view <- itself |
| Seen on screen | bottom view alternates near/far shot from ~35.0 s | smooth |
| `WR64_NO_SCENE_REGIONS=1`, same build | 34 frames, flicker back | -- |

**Riders** (`WR64_NO_MODEL_IDS=1` against the default):

| | heuristic | by identity |
|---|---:|---:|
| Attract demo race: moving riders with parts split between snapped and gliding | 140 | 59 -- all of them a water-level object beside the rider (a shadow or wake that appears and disappears), not a part |
| Attract demo race: parts paired by identity / snapped | -- | 79,362 / **0** |
| Why a part was left unpaired (heuristic, moving riders) | own previous pose taken by another part: 36; own previous under a different draw call: 14 | -- |
| Watercraft select, 8 switches: frames with an unpaired or snapped part | 16 (2-11 of 18-23 parts each switch) | 2, both a switch onto the 23-part rider, where the whole model takes its new pose together |
| Seen on screen | parts snapping in races and on switching riders (reported in play) | none, in races and on the select screen (confirmed in play) |

**Objects** (`WR64_PAIRING_MAX_JUMP=0` against the default):

| | limit off | limit 150 |
|---|---:|---:|
| 2P start: largest pair | 15,400 (title demo); 63 in the race | 120 |
| 1P race start: gliding pairs over 150 | 192 in 9 s, a row of markers 400 apart shifting one slot a frame | 0 |
| Attract demo: gliding pairs over 150 | 2,252 in 28 s, all one draw call | 0 |

**Seen on screen: the purple course-edge buoys.** Where the sliding shows is
the opening sequence before the attract demo race -- the camera flying over the
course with no input -- at maximum draw distance, with interpolation on: rows
of identical edge buoys, each paired with a neighbour 400 units along the row as
the game repacks its buoy table, slid along the row. With the limit, confirmed
fixed in play; the log of that sequence (1,410 frames) shows no pair over 150
units drawn and no still object glided sideways, with ~10,500 candidates a frame
refused. The captures of the demo race checked earlier caught no glide because
the rows gliding there were out of shot.

To reproduce: launch with no input script and look at the opening sequence --
nothing needs pressing -- with `WR64_PAIRING_MAX_JUMP=0` against the default.

**Cost**, 2P VS race, `WR64_FRAME_STATS=1`, the camera and object fixes off
against on: 19 two-second race windows each, the same rate in every window
(mean 20.26, 100% at the game's target). The rider identities off against on:
17 and 19 windows, mean 20.29 and 20.26, 100% at target in both. The object limit refuses up to ~22,000 candidates a
frame in the title demo -- RT64 scores every one of ~150 identical course
objects against every other -- before the expensive part of scoring them.

---

## Putting it in another project

Four pieces. The log and the capture are what make the other two findings
rather than guesses; build them first.

### 1. The pairing log

Where: `GameFrame::match()` in `src/hle/rt64_game_frame.cpp`, after
`matchScenes(...)` for both scene lists.

**Cameras.** `matchScenes` is a lambda that sorts candidate (current scene,
previous scene) pairs by `matrixDifference(view) + matrixDifference(proj)` and
calls `matchScene` for each accepted pair. Record the accepted pair there -- a
`thread_local` vector of `{curIndex, prevIndex, difference, perspective}`,
filled only while the log is open -- and write it after matching. **Do not try
to recover the decision afterwards** from `frameMap.workloads[w].viewProjections`:
that map is keyed by transform index, and two scenes on different framebuffers
with identical matrices share one, so the recovery reports the same previous
scene for both. It did here.

For each scene, the part of the screen it drew into is the union of
`workload.fbPairs[fb].projections[p].scissorRect` over its projections; a
projection's scissor is the union of its draw calls' (`Projection::addGameCall`).

**Objects.** `frameMap.workloads[w].transforms[t]` gives `mapped`,
`prevTransformIndex` and the rigid body. The translation is row 3 of
`drawData.worldTransforms[t]`. RT64 keeps no transform-to-call map, so build one
per workload by walking `fbPairs -> projections -> gameCalls` and folding
`transformGroups[worldTransformGroups[m]].matrixId` over each call's matrix
range as `buildCallHashMap` does, then `hashFromCall`. A call with no matrices
has `minWorldMatrix = USHRT_MAX`: test `min > max`.

Write a wall-clock time on each frame line. Without it a log frame cannot be
matched to a picture, and the picture is what decides.

### 2. The window capture

Windows Graphics Capture (`windows-capture` on PyPI) reads a window's content
through the compositor, so it is immune to whatever covers the game, and it
works on a D3D12 flip-model swap chain, where `PrintWindow` returns nothing.
Two things made it usable:

- **Do not encode in the frame callback.** Resizing and JPEG-encoding a 1080p
  frame there dropped delivery to 14 a second. Downscale with a linear filter
  in the callback, queue the array, encode on a writer thread: 30-40 a second.
- **Name frames by time since launch**, and record the launch time in the log's
  clock, so a log frame and a picture can be put side by side.

Presented above its logic rate, most frames on screen are generated -- at 20
game frames a second and 60 Hz, two in every three. A defect that exists only
on generated frames shows as an **alternation between consecutive captures**:
the picture keeps returning to one state and leaving it, which is exactly what
the burst looked like.

### 3. Cameras paired by screen region

Where: the candidate loop of `matchScenes`. Before `matchCandidates.emplace_back`
for current scene *i* and previous scene *j*, skip the pair unless:

1. both scenes' first projections have the same `fbPairIndex`, and those
   framebuffers the same `colorImage.fmt`, `.siz` and `.width`;
2. their screen regions -- the union of their projections' scissors -- are both
   empty, or overlap by at least **half the smaller one's area**.

Among what remains, the smallest matrix difference still wins; a scene with no
compatible previous scene stays unpaired and is drawn without interpolation.

Three details that matter:

- **Do not compare `colorImage.address`.** A double-buffered game alternates
  framebuffers every frame; comparing addresses would stop every camera from
  pairing. The slot in the frame is the identity.
- **Half the smaller area**, not equality: the same camera drawn inset in one
  frame and full-frame in the next (the transition into a menu's 3D box) must
  still pair.
- **Empty with empty.** Here a perspective projection on framebuffer slot 0
  draws nothing in every race frame; it took part in the rotation, and it
  should pair only with its own kind.

### 4. The object pair limit

Where: `computeTransformMatch()`, after the determinant test and **before** the
velocity prediction:

```cpp
const float rawJump = hlslpp::length(curPos - prevPos);   // raw translations
if (maxJump > 0.0f && rawJump > maxJump) return matchResult;  // .valid stays false
```

Raw, not predicted: the prediction adds the previous rigid body's velocity, and
after one wrong pair that velocity is the wrong jump. Pick the number from the
log: the largest legitimate move between two game frames (here ~60), with
margin, and below the spacing of the repeated objects (here 400).

### 5. Riders' parts paired by identity

This one is in the port, because only the display list knows which matrix is
which part. RT64's explicit id path (`GameFrame::match`, the walk over
`transformIdMap`) pairs every transform carrying id *k* with linear ordering
with the transform carrying *k* in the previous frame, in submission order,
before and instead of the heuristic.

**The identity is the matrix's segmented address.** Measured: a rider's parts
load from a fixed table (`0x0300E108 + part * 0x100 + rider * 0x40`), while the
list a rider is drawn from moved between segment 2 slots mid-run and the part
meshes change with the selected rider. Hash the address to 32 bits, set the
top bit, and keep clear of `G_EX_ID_IGNORE` (0) and `G_EX_ID_AUTO` (~0).

**Where the matrix loads are.** Two forms, both handled:

- *Inside a model list* (races): a top-level call to a segment 2 list that
  loads at least four modelview matrices. The rewriter emits calls and lets
  RT64 walk in, so it inlines the list -- copies its commands into the scratch
  list, nested calls left as calls -- and sets a group before each load.
- *At the top level* (the watercraft select screen): a plain modelview load
  whose next drawing command is a call into segment 8, the character bank.

**The group.** `gEXMatrixGroup` with the id, `G_EX_ORDER_LINEAR`,
`G_EX_INTERPOLATE_DECOMPOSE`, vertex and texture-coordinate components
`G_EX_COMPONENT_SKIP`, everything else `G_EX_COMPONENT_AUTO` -- **except the
translation, `G_EX_COMPONENT_INTERPOLATE`**. `AUTO` lets each part's rigid body
decide whether to glide from its own velocity history, and eighteen parts of
one body deciding separately is what pulls it apart; `INTERPOLATE` skips that
check. Put the defaults back (`G_EX_ID_AUTO`, `G_EX_ORDER_AUTO`) after the
model.

**Two exceptions, both `G_EX_COMPONENT_SKIP`** -- drawn at the new pose, not
interpolated:

- *A part that moved further than a part can in one game frame* (150 units): a
  respawn, or the table handed to another rider. Keep last frame's translation
  per address to tell.
- *A run of parts containing a part that was not loaded last frame*: a switch
  to a rider with more parts. A new part has nothing to glide from; if the rest
  glide, it is drawn apart from them. Look ahead over the run first, and step
  the whole model at once.

Split screen draws each rider twice under the same addresses. Linear ordering
pairs the n-th occurrence with the n-th, and both occurrences carry the same
matrix, so the order between the views does not matter.

### What was tried and removed

Kept here so the next attempt does not repeat it.

- **Ids for buoys and course props** from their exact position. Removed: the
  object limit covers the same pairs, and none of those glides was seen.
- **The rider identities, the first time.** They went in alongside the
  object limit as the fix for the 2P burst, changed nothing about the burst --
  that was the cameras -- and were taken out on the strength of one number: no
  object moved more than 63 units in the 2P intro, where the riders stand
  still. Racing at speed, with the log grouped by rider, showed parts snapping
  every few frames. They came back with the translation always interpolated and
  the two exceptions above, and the riders stopped coming apart. The lesson
  is the same as the burst's, from the other side: measure the defect **where
  the user sees it** -- here, riders moving -- not where it is convenient.

---

## Keeping this current

This file is the manual and the recipe for one piece of the port. When the
log's format, the switches, the limit's default, the scene rule, the rider
identity rules, the capture or the input scripts change, change this file in
the same commit. The mechanism and
the history stay in [PORTING.md](PORTING.md) §7; the game's facts -- split-screen
regions, the empty scene, the racers' matrices -- in
[GAME-INTERNALS.md](GAME-INTERNALS.md) §6.
