# The render-distance census

A way to find out, for a recompiled N64 game, **which kinds of object the game
stops drawing at a distance and which it does not** — and for the ones it does,
the number to go looking for.

The question matters because a "draw distance" setting in a PC port cannot do
anything a renderer-side change can reach. In Wave Race 64 the far plane is
already at 16,192, about twenty times further out than anything a course
contains; raising it reveals nothing. What limits the view is the game's own
culling, decided per kind of object, in that object's own code, **before
anything reaches a display list**. A renderer cannot put back what was never
submitted. So a correct setting is a list of the game's own limits, and the list
has to be measured before it can be scaled.

This document has two halves: how to run the census, and how to build the same
thing in another project.

---

## Using it

### Turning it on

Two environment variables, both read once at startup:

| Variable | What it does |
|---|---|
| `WR64_DISTANCE_CSV` | Path for the census CSV. Setting it puts the display-list tracer in census mode. |
| `WR64_3D_TRACE_FRAMES` | How many race frames to record (census mode defaults to 600). |
| `WR64_COURSE_STRUCT` | Path for the per-course struct dump, written once per course. |

```
cd build-fe
WR64_DISTANCE_CSV=../build/field/census.csv \
WR64_COURSE_STRUCT=../build/field/course-struct.csv \
WR64_3D_TRACE_FRAMES=6000 \
./WaveRace64Recomp.exe /path/to/rom.z64

python tools/render_distance_census.py build/field/census.csv
```

### Let the attract demo drive

**Do not script the inputs.** A script of timed button presses cannot see the
shore: the first attempt at this beached the craft on the first long lean and
the race was over in twenty seconds — 443 frames, one course, a camera that
barely moved. The attract demo drives the course properly, at racing speed,
around the whole lap, and then **moves on to another course by itself**. Passing
the ROM on the command line skips the launcher, and doing nothing after that is
the whole procedure.

That also satisfies the rule the measurement needs: a verdict that does not
reproduce on a second course is not a finding. Every CSV row carries its course
number and the analysis is per course, so one unattended run covers several.

A run takes about ninety seconds to reach the first demo and roughly forty
seconds of racing per course, so twenty-five minutes covers four or five.

### What the CSV holds

One row per display-list call on a traced frame:

```
frame,course,list,texture,x,y,z,distance,camx,camz
```

* `list` and `texture` are **segmented addresses into fixed segments**, which is
  what identifies an object between sessions. A segment-3 offset identifies
  nothing: it only says where this frame's allocator happened to land.
* `x,y,z` is the translation of the matrix in force — the object's world
  position, because object matrices are *loaded* rather than composed with a
  view.
* `distance` is the **horizontal** distance from the game's own camera, because
  that is the metric the game's own culls use: the buoy test squares dx and dz
  and leaves dy out. A census measuring something else is not comparable with
  the limit it is looking for.

### Reading the report

```
=== course 1: 2604 frames, 575 kinds, camera x -6784..6630, z -1481..1346 ===
  verdict      reach  max drawn  furthest  drawn<=reach  frames  sites   beyond  list / texture
  CULLED        4997       5464     13757           95%    2456     39    46585  0x0102CD78 0x01021978
```

* **reach** — the 99.5th percentile of the distances it was drawn at.
* **skips near** — how often a further instance was drawn while a nearer one was
  left out; see below, and do not read it as a disqualifier.
* **max drawn** — the furthest single draw, which overshoots; see below.
* **furthest** — the furthest any of its sites ever was from the camera.
* **drawn<=reach** — of the frames where a site was within the reach, how many
  drew the kind.
* **beyond** — how many (frame, site) pairs sat beyond the reach and were not
  drawn.

The row above is the buoys, and it is the calibration case: their limit is known
to read **5,000**, and the census recovers 4,997 without being told.

| Verdict | What it means |
|---|---|
| `CULLED` | Drawn in most frames where it is close, never once where it is far. `reach` is the limit, and Stage 4 goes looking for the number. |
| `unclear` | Never drawn far, but not reliably drawn close either. Something other than distance is gating it — facing, occlusion, a racing line — and this cannot tell which. |
| `not culled` | Drawn as far as the run ever put it. Nothing here for a setting to scale. |
| `never far` | The run never got far enough away for a limit to show. Capture more. |
| `moves` | Its positions are not fixed — another racer, spray, part of the player's craft. The test does not apply. |
| `at origin` | Drawn under no world matrix at all: the HUD, the sky, a full-screen effect. The census has nothing to say about it. |

### Four traps this already fell into

**The cull is applied when the list is built, not when it is drawn.** A buoy
accepted at 4,999 is still in the list a frame or two later, when the camera
has moved further off, so a thin tail of draws always lands past the limit:
over 23,526 buoy draws the maximum is **5,464** against a limit of **5,000**,
a 9% overshoot. That is why the reported reach is a high percentile rather
than the maximum -- the 99.5th percentile of the same draws is 4,997. It
reads 5,969 on a course whose limit is 6,000 and 9,916 on a class whose limit
looks like 10,000. The maximum is still printed, because the size of the tail
is worth seeing.

**"It skips a nearer one" does not mean there is no cap.** The report carries a
`skips near` column: the share of frames in which the kind drew a further
instance while leaving a nearer one out. No "closer than N" rule can do that on
its own, so it is tempting to read a high share as proof the kind is *selected*
rather than culled -- the way this game picks its direction arrows by the racing
line. That reasoning is wrong here, and the buoys are the counter-example: their
cap is proven causally, by doubling the number and watching the reach double,
and they still skip a nearer buoy in **92%** of frames. The game caps by
distance *and* chooses among what is inside the cap. A verdict built on this
column reclassified the buoys and would have thrown the causal result away, so
it is reported and not ruled on.

**`reach` is a lower bound, not the limit.** A kind with forty sites spread along
the course has one sitting at the limit in most frames, and its reach lands
within a unit of the real number. A kind with one site reaches only as far as
that site happened to get. Reaches of 4,999, 4,992, 4,984 and 4,972 on the same
course are most likely one limit of 5,000 sampled four times, not four limits.

**A steady maximum is not a cull.** The first version of this watched the
per-frame maximum and called a kind culled when it held steady over a run. That
works for the buoys, which line the whole course, and is worthless for anything
else: four kinds were reported culled at exactly 2,572, and all four turned out
to be drawn at the world origin under no matrix — their "distance" was the
camera's distance from (0,0,0). Reach against opportunity replaced it because it
asks a question a lone object can answer.

### The per-course struct dump

`WR64_COURSE_STRUCT` writes the struct the buoy cull reads its limit from, once
per course, every field as a signed integer, a float and raw hex — the type is
not known in advance, and a distance reads plainly in one and as noise in the
other.

Confirmed fields of it so far:

| Offset | Course 0 | What |
|---|---|---|
| `+0x88`–`+0x94` | 255, 255, 255, 255 | Fog RGBA |
| `+0x98` | 990 | Fog near |
| `+0x9C` | 1000 | Fog far |
| `+0xA0` | 400 | — |
| `+0xA4` | **5000** | The buoy cull distance |

**The fog pair is not in world units.** 990 and 1000 are the N64's own
0–1000 normalised depth range, fed to `gSPFogFactor` as
`(500 * 256) / (far - near)`. It is worth writing down because "fog far is the
draw distance" is the first guess anyone makes, and here it is wrong by three
orders of magnitude.

---

## Putting it in another project

Five pieces, none of them large.

### 1. A row per display-list call

The port already walks the display list each frame for other reasons. The census
adds one `fprintf` at the `G_DL` case, guarded by a `FILE*` that is null unless
the environment variable is set. The tree output the walker normally prints is
redirected to the null device rather than guarded command by command — the walk
is shared, and a dozen conditionals in it would be a dozen chances to get one
wrong.

### 2. The world position: read the matrix in force, not the stack

The translation of the modelview matrix in force is the object's world position
**when object matrices are loaded rather than multiplied**, which is the common
case for static scenery. Two earlier models were wrong and both failed the same
way, with every static object at a constant distance:

* *"The modelview holds view × model, so its translation is the eye-space
  position."* It does not; the view is not in the stack at all.
* *"The first modelview loaded in a frame is the view matrix."* It is not.

If a game does compose a view into the stack, this needs inverting properly —
but check the symptom first: a distance that never changes while the camera
moves is the signature of getting this wrong.

### 3. The camera: read the game's own

Do not derive it. The game keeps it somewhere, and the cull being measured reads
it from there:

```c
// gCameraPerspective at 0x80227C80, stride 0x10C, world position at +0x4C,
// which is exactly what the buoy cull reads at 0x8006EC30.
const uint32_t index = read_word(rdram, 0x00223930) & 3;
const uint32_t base  = 0x00227C80 + index * 0x10C;
camera_x = read_float(rdram, base + 0x4C);
camera_z = read_float(rdram, base + 0x54);
```

Using the same position the game's own cull uses is what makes a measured
ceiling comparable with the limit behind it.

### 4. Reach against opportunity

The analysis is fifty lines and needs no libraries. For each `(display list,
texture)` pair on one course:

1. Collect the **sites** — the distinct positions it was ever drawn at. If they
   outnumber half the draws, the object moves and the test does not apply. If
   they are all at the origin, it is drawn under no matrix and the test does not
   apply.
2. `reach` = a high percentile of the distances it was actually drawn at.
   Not the maximum: the cull runs when the list is built and the measurement
   happens when the list is drawn, so a few draws always land past the limit.
3. `opportunity` = the furthest any site ever was from the camera, over every
   frame of the run — **including the frames where it was not drawn**. This is
   the whole trick: the camera is recorded in every row, so the distance to a
   static site is known whether or not the object appeared.
4. If `reach` is far below `opportunity` and many pairs sat beyond it unused, it
   is culled at `reach`.

Thin both sets to an even stride before the double loop — frames × sites is
billions on a long run, and an evenly spaced few hundred of each keeps the
extremes. Even spacing, not the first N: the first N cameras of a run are one
corner of the course, and a subset that never leaves it reports that nothing
ever got far away.

### 5. Dump the struct the known cull reads from

Once one limit is found, its neighbours are the cheapest place to look for the
next. Follow the same pointer the game's own code dereferences rather than
indexing an array — no stride to get wrong — and dump on a **course change**,
not a pointer change: this game leaves the pointer at the array's base for every
course, so a dump waiting for it to move dumps once and never again.

---

## Files

| Path | What |
|---|---|
| `src/dlrewrite.cpp` | Census mode in the display-list walker; `dump_course_struct` |
| `tools/render_distance_census.py` | The analysis and the verdicts |
| `tools/scripts/race.txt` | A scripted race — useful for menus, *not* for the census |
| `include/wr64/drawdistance.h` | The limits found so far, and what has been ruled out |
| `docs/GAME-INTERNALS.md` | §6, the game facts the census produces |

## Keeping this current

This is a measurement tool, so its documentation goes stale in a particular way:
the **method** changes rarely and the **verdicts** change every time a capture is
taken. Verdicts belong in `include/wr64/drawdistance.h` and
`docs/GAME-INTERNALS.md`, which is where a reader looks for "what is the buoy
limit"; this file holds the method and the traps. When the census gains a
verdict class or a column, both halves above change together — the "using it"
table and the recipe — because someone porting the idea needs the column to mean
the same thing.
