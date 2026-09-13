# Transform pairing: matrix interpolation that pairs the right objects

RT64 draws the frames between the game's by pairing each world transform of a
frame with one of the previous frame's and moving the object between the two.
When it pairs the wrong two, the object is drawn sliding from where a different
object was. In this game that was buoys sliding as they came into view, and the
riders and course scenery bursting apart for a second at the start of every
race.

Three parts, each measured: a **pairing log** that names every pair RT64 made,
a **jump limit** in RT64's matcher that refuses a pair no object could have
made, and **identities** the display-list rewriter gives the objects RT64 was
confusing, so it pairs them by name instead of by guess.

Two halves: [**using it**](#using-it) and [**putting it in another
project**](#putting-it-in-another-project). The mechanism, and how it was
found, are in [PORTING.md](PORTING.md) §7, *Interpolation*; the game facts the
identities rest on are in [GAME-INTERNALS.md](GAME-INTERNALS.md) §6.

---

## Using it

### The switches

Everything is on in every build. Each part has a switch, because whether a
change is an improvement is a question only a side-by-side can answer:

| Environment variable | Effect |
|---|---|
| `WR64_PAIRING_MAX_JUMP=<units>` | The jump limit, in world units. Built in at **150**; `0` switches it off. |
| `WR64_NO_SITE_IDS=1` | No identities for fixed sites (buoys, gate markers, rings, props). |
| `WR64_NO_MODEL_IDS=1` | No identities for animated models (the racers); their lists are left as calls. |
| `WR64_PAIRING_LOG=<file>` | Write the pairing log. Nothing is written without it. |
| `WR64_PAIRING=1` | The older two-second counter of unpaired transforms, in the log beside the frame rate. |

The one check worth running before any of them: present at the game's own
frame rate, so that no frames are interpolated at all. Anything that survives
is not interpolation; anything that disappears is nothing else.

### The pairing log

`WR64_PAIRING_LOG` names a file. RT64 then writes, for every game frame it
matched, one line per world transform:

```
F 900
T 3 3 auto id=FFFFFFFF call=A19C9E2B0213B85E range=3-3 cur=4400.0,0.0,-2000.0 prev=4400.0,0.0,-2000.0 jump=0.0 lerp=1 pvel=0.0
T 62 62 id id=8A1F00C3 call=77251B70F2A0FE72 range=62-62 cur=-2302.1,-0.8,824.2 prev=-2302.1,-2.9,824.1 jump=2.1 lerp=1 pvel=2.1
U 0 id=FFFFFFFF call=4C55F4BEBF41AB80 range=0-0 cur=0.0,0.0,0.0
S frame=900 total=197 paired=196 j50=0 j100=0 j200=0 max=6.2 refused=0
```

| Field | Meaning |
|---|---|
| `F n` | A new game frame; `n` counts matched frames since the process started. |
| `T t p path` | Transform `t` was paired with previous-frame transform `p`. `path` is `id` when the pair came from an explicit id with linear ordering, `auto` when it came from the call-hash heuristic. |
| `U t` | Transform `t` found no pair. It is drawn at its current matrix, which cannot tear anything. |
| `id=` | The transform's group id: `FFFFFFFF` is `G_EX_ID_AUTO`, anything else was set by the port. |
| `call=` | RT64's own 64-bit hash of the draw call the transform sits in: combiner, other modes, geometry mode, triangle count and the folded matrix ids. Two calls with the same hash are candidates for each other. |
| `range=` | The world-matrix indices the call spans. A call spanning several matrices pairs them by index. |
| `cur=`, `prev=` | The two translations, world units. |
| `jump=` | The distance between them. The number the whole thing is about. |
| `lerp=` | Whether the rigid body agreed to move the object between them (`updateLinear`): it refuses a velocity more than ten times the previous one, above 5 units. |
| `pvel=` | The previous frame's velocity for the paired transform -- what the rigid body judged this jump against. A large `pvel` on a small jump is a velocity left behind by an earlier wrong pair. |
| `S` | Per-frame summary: transforms, pairs, pairs over 50/100/200 units, the largest, and how many candidates the jump limit refused. |

The log is written on RT64's workload thread, one `fprintf` per transform,
flushed per frame. A race frame is 60-200 lines; a two-minute run is 12-20 MB.
It slows nothing that a measurement of it would notice, but it is not free:
take it off for a frame-rate measurement.

### Reading it

`tools/pairing_log.py` does the reading:

```
python tools/pairing_log.py pairing.log                       # totals, distribution, frames with a jump over 100
python tools/pairing_log.py pairing.log --over 50 --lines     # a lower bar, and the raw lines
python tools/pairing_log.py pairing.log --frames 700-1400     # only those frames
python tools/pairing_log.py pairing.log --calls               # every draw call: pairs, pairs over the bar, matrix range
```

The default report:

```
frames 1-2515: 2515 frames, 53.7 transforms a frame, 51.9 paired (30.3 by id), 175.44 candidates refused a frame
jumps of every pair: p50 0.7  p90 48.9  p99 56.3  p99.9 62.6  max 143.1
  over   25:  34927 pairs in  1064 frames
  over   50:  11407 pairs in   448 frames
  over  100:      4 pairs in     4 frames
  over  200:      0 pairs in     0 frames
```

What to look at, in order:

1. **`max`, and the `over 200` row.** Nothing in this game moves more than 60
   units between two game frames. Every pair above that is wrong, and the
   frames listed under it say which object and when.
2. **`by id`.** How much of the frame is being paired by identity rather than
   guessed. Racing, with the identities on, about 40 of 68 transforms.
3. **`--calls`.** A repeated object -- every buoy, every spray particle, every
   limb of every racer of one model -- shares one hash. The table's `width`
   column says whether RT64 merged a run of them into one call (paired by
   index; slot shifts show here) or kept them apart (paired by nearest score;
   cross-pairing shows here).
4. **A cascade.** After a wrong pair, the same transform's `pvel` carries the
   jump into the next frame. Ten frames of pairs in the hundreds with `pvel`
   in the thousands is a cut that was never recovered from; see the numbers
   below.

The 3D trace (`WR64_3D_TRACE`) is the other half of a diagnosis: it lists the
game's matrix loads with their translations, and a transform in the log is
found in it by position.

### What the numbers were

All from `tools/scripts/race.txt`, a scripted Time Trial on the first course,
one player, on the port's own machine. The two runs are the same script with
the fixes off and on.

**Before**, 701 race frames:

| | |
|---|---|
| Pairs that jumped over 100 units | **631**, in 101 frames |
| Over 400 | 471 |
| Largest jump | **10,088** units |
| Largest legitimate per-frame move (p99 of all pairs) | 57.9 |
| The two camera cuts before the race (frames 745 and 795) | 14 and 46 pairs over 100 in the cut frame, then a **cascade of 10-15 frames** in which buoys and gate markers paired with neighbours 400-1,600 units away with the lerp *accepted*, `pvel` in the thousands |
| Between cuts | a buoy near the camera loses its own match every few frames and takes one thousands of units away; a buoy entering the table takes whichever previous buoy was left over |

**After**, the whole run, 2,515 frames including the menus, the cuts and
the results screen:

| | |
|---|---|
| Pairs over 100 | **4**, none over 200 |
| Largest jump | 143.1 -- a spray particle |
| Paired by identity | 30.3 of 51.9 transforms a frame; racing, 40 of 68 |
| Draw calls still paired by the heuristic, racing | 2: the spray, and a moving underwater object |
| Candidates refused by the limit | 175 a frame over the run, 627 racing: the spray clouds of four racers are single-matrix calls with one hash, so every particle is a candidate for every other, and the limit now cuts those before they are scored |
| At the race-start cut (frame 795) | 88 of 120 transforms paired -- the buoys, markers and limbs by id -- and 32 refused and drawn in place for one frame; the next frame 119 of 120, no cascade |

Restating the "before" column in the same terms: in that run the cut frame
paired 127 of 137 and the *wrong* 46 of them were then carried for fifteen
frames.

**Cost**, measured as the sea extension was (`WR64_FRAME_STATS=1`, the same
100 seconds of the attract sequence, two-second windows), fixes off against
on:

| | off | on |
|---|---:|---:|
| Windows | 48 | 48 |
| Mean rate | 19.94 | 19.94 |
| Windows at the game's own 20 | 97.9% | 97.9% |

The one window below target in each run is the first, during boot. The
metric saturates at the game's rate, so what this says is that the port
still keeps up, not how much headroom was spent; the work itself is a hash-set
insert and lookup per top-level matrix load, two extra commands per tagged
load, and a few hundred copied commands per racer per frame.

---

## Putting it in another project

Three pieces, in the order they were built. The first is worth building alone:
it is what makes the other two arguments rather than guesses.

### 1. The pairing log

Where: `GameFrame::match()` in `src/hle/rt64_game_frame.cpp`, after the
scene matching. At that point `frameMap.workloads[w].transforms[t]` says, for
every world transform of every workload of the frame, whether it was `mapped`
and to which `prevTransformIndex`, and the rigid body that will interpolate it.
The port's patch is `tools/patch_rt64.py`, `PAIRING_LOG_*`; the whole thing
is about 90 lines and touches nothing RT64 does.

Per transform you want:

- its translation and the paired one's -- `drawData.worldTransforms[t]` row 3;
  `memcpy` the matrix into `float[16]` and read `[12..14]`;
- its group: `drawData.transformGroups[drawData.worldTransformGroups[t]]`, for
  the id and ordering, which say which path paired it;
- the call it sits in and that call's hash. RT64 does not keep a
  transform-to-call map, so build one per workload: walk
  `fbPairs[f].projections[p].gameCalls[c]`, and for each call fold the ids of
  matrices `minWorldMatrix..maxWorldMatrix` exactly as `buildCallHashMap` does
  (`hash = hash * 33 ^ group.matrixId`) and call `hashFromCall(call, hash)`;
- the previous velocity: `prevFrame.frameMap.workloads[prevIndex]
  .transforms[p].rigidBody.linearVelocity`, guarded by `prevFrame.matched`.

Two traps:

- A call with no matrices has `minWorldMatrix = USHRT_MAX` and
  `maxWorldMatrix = 0`. Test `min > max` before looping.
- The log is written from the workload thread; open the file once, from a
  function-local static, and read the environment there too. `fflush` per
  frame, or a process killed from outside loses the frames you wanted.

### 2. The jump limit

Where: `computeTransformMatch()` in the same file, before the velocity
prediction. RT64 accepts any candidate pair whose determinants agree in sign;
there is no notion of an impossible distance. Add one:

```cpp
const float rawJump = hlslpp::length(curPos - prevPos);   // raw translations
if (maxJump > 0.0f && rawJump > maxJump) return matchResult;  // .valid stays false
```

Raw, not predicted: the prediction adds the previous rigid body's velocity to
`prevPos`, and after one wrong pair that velocity is the wrong pair's jump, so
a predicted distance would make the *next* wrong pair look plausible. A
refused candidate leaves the transform unpaired if nothing else claims it,
and an unpaired transform is drawn at its current matrix: one step, no slide,
and a clean rigid body for the next frame.

Choosing the number: log a run, take the largest legitimate per-frame move
(`p99` of all pairs is a fair proxy; here 58), and give it margin -- 150 is
two and a half times that. Keep it below the spacing of the repeated objects
the heuristic confuses (400 for the buoys here), or it will not refuse the
pair it exists for.

Why the limit is needed even though RT64's rigid body already refuses
implausible velocities: `updateLinear` still *records* the refused jump as
the velocity. The next frame the same object moves a little, the ratio is
now tiny, the lerp is accepted, and the object slides -- from where the
wrong object was. The limit stops the pair before anything is recorded.

### 3. Identities

RT64 pairs an explicit transform id with **linear ordering** by identity,
before and instead of the heuristic (`GameFrame::match`, the walk over
`transformIdMap`): every transform with id *k* in this frame is paired with a
transform with id *k* in the last, in submission order. Nothing else about it
changes if the group carries the defaults otherwise. That is the whole
mechanism; what remains is choosing identities that are the same for the same
object in every frame and never the same for two objects, and a place to set
the group.

The group is `gEXMatrixGroup` with `G_EX_INTERPOLATE_DECOMPOSE`, every
component `G_EX_COMPONENT_AUTO` except vertices and texture coordinates at
`G_EX_COMPONENT_SKIP` -- RT64's defaults, read from
`rt64_transform_group.h` -- with the id and `G_EX_ORDER_LINEAR`. Set it
before the matrix load: RT64 creates the transform at the first vertex after
the load, under whatever group is in force. Put the defaults back (`G_EX_ID_AUTO`,
`G_EX_ORDER_AUTO`) before the next load that is not yours, and at the end of
the list.

Two rules, each from one thing this game does:

**Fixed sites -- an object drawn from a repacked table.** The buoys, markers
and props are one plain matrix load plus one call to a static list each, from
a table the game repacks every frame, so a slot is not an identity. The
position is: the game writes it from the same numbers every frame, so hash
the X and Z translation's *bits* (not a rounding -- a rounding boundary makes
one place two identities) together with the called list's address. Tag a load
only if that key was also seen in the previous frame: an object that moves
would get a new key every frame and never pair at all, so a moving object
keeps the heuristic, and an object's first frame at a place keeps it too. The
rewriter keeps two hash sets and swaps them per list. Conditions here: a plain
load (`G_MTX_LOAD` without `G_MTX_PUSH`), a race frame, a perspective
projection, and the next drawing command after the load is a call to a list
in a cartridge segment -- a racer's root is a load with a push that calls a
list in RDRAM, and must not be touched. Look ahead a few commands from the
load to find the call; the walk has the list in RDRAM.

**Animated models -- matrices the port cannot reach.** A racer is a call to a
per-frame list with its limbs' matrix loads *inside*, and the rewriter emits
calls and lets RT64 walk in. So inline the call: copy its commands into the
scratch list, nested calls left as calls (they draw vertices only), and set a
group before each matrix load. The identity is the **matrix's segmented
address**: measured over eight frames, a racer's limbs sit at fixed addresses
in the game's arena, while the *list* a racer is drawn from moved between two
slots mid-run, so neither the list's address nor a load's ordinal in it would
do. Conditions: a top-level call to a segment 2 list, race frame, perspective,
and the list loads a matrix at all. Track the modelview stack through the
inlined loads as the top level would.

What not to tag: particles. A racer's spray is a few dozen single-matrix draws
rebuilt every frame, moving 15-75 units and shuffling; nothing names one, and
a wrong pair within the cloud is a particle's own size. The jump limit is what
covers them.

### Checking it

The log, before and after, with the target of **no pair over the limit outside
a scene cut**, and `by id` covering the objects you tagged. Then the two things
a log cannot show: play the moment that was wrong at the display's rate, and
measure the frame rate with `WR64_FRAME_STATS=1` before and after -- the
identities cost a hash-set lookup per top-level matrix load and a few hundred
copied commands per racer per frame, and the limit replaces a score with a
comparison, but "negligible" is a measurement, not an adjective.

---

## Keeping this current

This file is the manual and the recipe for one piece of the port. When the
pairing log's format, the switches, the limit's default or the identity rules
change, change this file in the same commit. The mechanism and the wrong turns
stay in [PORTING.md](PORTING.md) §7; the game's own facts -- the buoy table,
the racers' matrices -- in [GAME-INTERNALS.md](GAME-INTERNALS.md) §6.
