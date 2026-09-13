# Transform pairing: interpolation that pairs the right cameras and objects

RT64 draws the frames between the game's by pairing what it sees in one frame
with the previous frame's -- **cameras** first, then the **objects** drawn under
each camera -- and moving everything between the two. When it pairs the wrong
two, the picture is drawn from somewhere it never was.

Two defects here, and the fix for each, both inside RT64
(`tools/patch_rt64.py`):

| Defect | What was wrong | Fix |
|---|---|---|
| **The 2P race-start burst.** For over a second after a two-player race fades in, each view flickers between two shots. | For 33 consecutive game frames each view's camera was paired with *another* view's previous camera. | A camera only continues a previous camera on the same framebuffer slot that drew into mostly the same part of the screen. |
| **Objects gliding across the course.** Repeated objects -- buoys, markers, spray -- paired with one hundreds to thousands of units away and drawn sliding between them. | RT64 accepts any object pair, whatever the distance. | A pair further apart than 150 world units is refused. |

And the instruments that found them: a **pairing log** that records every
camera and object pair, a **window capture** that records every presented
frame even with something on top of the game, and a **two-player input
script**.

Two halves: [**using it**](#using-it) and [**putting it in another
project**](#putting-it-in-another-project). The mechanism and the wrong turns
are in [PORTING.md](PORTING.md) §7, *Interpolation*; the game facts in
[GAME-INTERNALS.md](GAME-INTERNALS.md) §6.

---

## Using it

### The switches

Both fixes are on in every build.

| Environment variable | Effect |
|---|---|
| `WR64_NO_SCENE_REGIONS=1` | Cameras are paired by matrix alone again, as upstream RT64 does. The 2P burst comes back. |
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
pillow`. `--env NAME=VALUE` sets a variable for that run only.

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
| `U t` | No pair: drawn at the current matrix, cannot tear. |
| `S` | Frame summary; `refused` = object candidates the jump limit turned away. |

A two-player race frame is ~300 lines; a two-minute run 12-20 MB.

### Reading it

```
python tools/pairing_log.py pairing.log --scenes        # camera pairs across framebuffer slots or screen regions
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

**Objects** (`WR64_PAIRING_MAX_JUMP=0` against the default):

| | limit off | limit 150 |
|---|---:|---:|
| 2P start: largest pair | 15,400 (title demo); 63 in the race | 120 |
| 1P race start: gliding pairs over 150 | 192 in 9 s, a row of markers 400 apart shifting one slot a frame | 0 |
| Attract demo: gliding pairs over 150 | 2,252 in 28 s, all one draw call | 0 |

The gliding pairs were **not caught on screen**: in every case checked the
objects were out of shot or too small -- spray, a row of markers behind the
close-up camera. The limit is kept because a pair further apart than anything
in this game can move is wrong by construction, and it is cheaper than scoring
it; but it is the camera fix that ended the burst.

**Cost**, 2P VS race, `WR64_FRAME_STATS=1`, both fixes off against on: 19
two-second race windows each, the same rate in every window (mean 20.26, 100%
at the game's target). The object limit refuses up to ~22,000 candidates a
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

### What was tried and removed

Kept here so the next attempt does not repeat it. Giving buoys ids from their
exact position, and inlining each racer's display list to give every limb an
id from its matrix address, paired those objects by identity and cut logged
object jumps over 100 units from 631 to 4. **It changed nothing on screen.** In
a 2P start with the camera fix alone, no object pair moved more than 63 units:
the racers were never mispaired, and the limit covers the repeated scenery. A
log number improving while the symptom does not is the counter measuring
something else -- which this project had already written down once.

---

## Keeping this current

This file is the manual and the recipe for one piece of the port. When the
log's format, the switches, the limit's default, the scene rule, the capture or
the 2P script change, change this file in the same commit. The mechanism and
the history stay in [PORTING.md](PORTING.md) §7; the game's facts -- split-screen
regions, the empty scene, the racers' matrices -- in
[GAME-INTERNALS.md](GAME-INTERNALS.md) §6.
