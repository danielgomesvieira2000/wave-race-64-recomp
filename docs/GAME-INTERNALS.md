# Wave Race 64 internals

Everything this port had to learn about the game itself, in a form a
decompilation or another port can use directly. It is a reference, not a
narrative; how each fact was established is in the phase findings
(`docs/findings/phase-01.md` through `docs/findings/phase-05.md`).

The companion document, [PORTING.md](PORTING.md), covers the toolchain and
runtime side -- N64Recomp, librecomp/ultramodern, RT64 -- which is game
independent.

**Every address here is Wave Race 64 (USA) (Rev A) vram unless stated
otherwise.** They come from an assembly-only ELF verified byte-identical to the
cartridge across all 32 segments, so a symbol's address is the linker's, not an
estimate. Function names follow LLONSIT's decompilation where it has one.

---

## 1. Cartridge identity

| Field | Value |
|---|---|
| Internal name | `WAVE RACE 64` |
| Cartridge ID / region / revision | `WR` / `E` (USA) / `1` (Rev A, "v1.1") |
| Header CRC1 / CRC2 | `0x492F4B61` / `0x04E5146A` |
| sha1 | `508dfc2d4caa42b6f6de5263d0aed5e44ac7966a` |
| XXH3-64 (what librecomp checks) | `0x2B675E2250A604FC` |
| Size | 8 MiB `.z64` |
| Entry point (header `0x08`) | **`0x80046800`** |
| Save device | 4 Kbit EEPROM |
| RDRAM the game uses | 4 MB; it never reads `osMemSize` |

The entry point is worth reading rather than assuming: it is not the usual
`0x80000400`.

Because the game never reads `osMemSize`, a runtime that reports 8 MB leaves the
upper 4 MB entirely unused by the game and free for a port's own scratch. This
port puts a display-list staging buffer at physical `0x700000` and a private copy
of the audio command list at `0x7E0000`.

### Rev A versus v1.0 (revision 0)

The two revisions are the same codebase but are **not** related by a constant
offset; Rev A inserted code at several points. Measured over the 221
`func_<hex>` symbols of `codeseg`:

| vram range (Rev A) | delta to v1.0 | symbols |
|---|---:|---:|
| `0x801DB430`-`0x801E3EE0` | `-0x2A0` | 86 |
| `0x801E4C08`-`0x801E6F6C` | `-0x2A0` | 12 |
| `0x801E71A8`-`0x801E8B24` | `-0x2A0` | 13 |
| `0x801E92FC`-`0x801EB180` | `-0x278` | 15 |
| `0x801EB4F4`-`0x801F25E0` | `-0x270` | 70 |
| `0x801F4120`-`0x801FC840` | `-0x248` | 15 |

The v1.0 data region sits `0x240` bytes earlier than Rev A's. Only 3.3% of Rev A
function addresses land on a v1.0 function boundary unmodified. Porting a symbol
corpus between the revisions is therefore a sequence-alignment problem -- function
*order* is preserved -- and not address arithmetic. `codeseg`'s ROM base of
`0xA95D0` is a Rev A value and does not hold for v1.0.

---

## 2. Code layout

| Region | ROM | vram | Size | Notes |
|---|---|---|---|---|
| `entry` | `0x1000` | `0x80046800` | `0x50` | boot: sets `$sp`, jumps to the boot proc |
| `main_segment` | `0x1050` | -- | `0xA5500` | resident; text **and** data in one section |
| `codeseg` | `0xA95D0` | `0x801DAFA0` | `0x4CAC0` | resident, DMA'd in at boot by `GameLoad_LoadCodeseg` |
| overlays | various | **`0x802C5800`** | up to `0x3D80` | 19 sections share this one address |

`main_segment` and `codeseg` together hold 1,088 functions, of 1,346 in the whole
image. The 19 sections at `0x802C5800` are sixteen numbered overlays plus
`segment_1B1FB0`, `seg_1C3780` and `seg_1C3D00`. Because they share an address, a
call target address alone cannot identify the callee -- only what is resident at
the time can.

The assembled ELF carries 22 `.rel.*` relocation sections: one per overlay, plus
`main_segment` and `codeseg`. That is what makes real overlay relocation
possible; both earlier public recompilation attempts had to disable it because
their ELF carried none.

---

## 3. Boot and the main loop

`SysMain_Thread` runs one iteration per game frame. The order matters, and it is
not the intuitive one:

```
800471E0: jal func_800922E4          ; per-frame update: controllers, screen state machine
800471FC: jal func_80092CF0          ; renderer dispatch (takes gDisplayListHead, returns it advanced)
80047208: jal SysMain_GfxFullSync    ; appends the full-sync command
8004727C: jal game_dma_copy          ; asset staging
80047290: jal unk_game_load          ; stages what the next state needs
800472A4: jal GameLoad_LoadOverlay   ; loads the overlay for the current state
```

**The renderer runs before the loader.** An overlay requested on one iteration is
used on the next; the load is always one step behind the use. On hardware that is
harmless. In a port it means the very first dispatch can target an overlay that
nothing has loaded yet, and a recompilation that resolves calls dynamically must
tolerate that ordering rather than treat it as an error.

`func_80092CF0` receives `gDisplayListHead` in a delay slot and its return value
is stored straight back:

```
800471FC: jal func_80092CF0
80047200:   lw $a0, %lo(gDisplayListHead)($a0)   ; delay slot: head in
80047208: jal SysMain_GfxFullSync
8004720C:   sw $v0, %lo(gDisplayListHead)($at)   ; delay slot: head out
```

Any stub of that function must return its argument unchanged, or the next
`SysMain_GfxFullSync` writes through a null pointer.

### The three functions that call into overlays

| Function | Distinct overlay targets | Role |
|---|---:|---|
| `func_80092CF0` | 20 | renderer dispatch on `gGameState` through `jtbl_800EAFA8` (104 entries, 302 instructions) |
| `func_800922E4` | 3 | per-frame update; its overlay calls are gated on `D_801CE638` being 1, 8 or `0x15` |
| `PauseMenu_Update` | 1 | pause menu |

Those three functions, across 24 call sites, are the whole of the resident code's
direct `jal`s into the overlay window. Nineteen of the call sites -- 14 in
`func_80092CF0` and 5 in `func_800922E4` -- target the overlay entry point
`0x802C5800` itself. Found by scanning function bodies, not whole sections; see
[PORTING.md §3](PORTING.md#3-running-n64recomp) for why that distinction matters.

---

## 4. The state machine

| Symbol | Address | Meaning |
|---|---|---|
| `gGameState` | `0x800DAB24` | which screen the game is on |
| `gGameModes` | `0x801CE620` | mode: 0 time trials, 1 2P vs, 4 championship, 11 stunt |
| `gGameModeState` | `0x801CE650` | sub-state within a mode |
| `D_801CE638` | `0x801CE638` | screen state; gates `func_800922E4`'s overlay calls |
| `D_800D461C` | `0x800D461C` | VI frame divider (see §8) |
| `gDisplayListHead` | `0x80151944` | see §6 |

`gGameState` values observed in play:

| | | | |
|---|---|---|---|
| `0x02` TITLE_SCREEN | `0x03` MAIN_MENU | `0x05` BOOT_UP | `0x07` DEMO (attract) |
| `0x0A` RIDER_SELECT | `0x14` COURSE_SELECT | `0x1E` COURSE_OVERVIEW | `0x28` TIME_TRIAL / race |
| `0x32` TIME_TRIALS_RESULTS | `0x34` RACE_RESULTS | `0x38` STUNT_MODE_RESULTS | `0x3C` OPTIONS_MENU |
| `0x3E` OPTIONS_CHANGE_NAMES | `0x40` OPTIONS_SAVE_AND_LOAD | `0x42` OPTIONS_VIEW_RECORDS | `0x44` OPTIONS_CHANGE_CONDITIONS |
| `0x46` OPTIONS_ERASE_COURSE_RECORDS | `0x48` OPTIONS_AUDIO | `0x66` CEREMONY | |

The game keeps around a dozen states in the neighbourhood of a race -- `0x28`
through `0x2D` at least, by the decompilation's enumeration -- so **a hardcoded
list of "race states" is the wrong thing to key behaviour on.** §6 gives a test
that reads the frame instead.

**Nothing stores to `gGameState` directly.** Across the whole disassembly the
symbol appears only in `lui` (123 times), `lw` (75) and `addiu` (48), and never
in a store: it is always written through a computed pointer, and every writer
lives in `codeseg`. `func_801EB180` is one of them:

```
801EB180: lui   $v0, %hi(gGameState)
801EB184: addiu $v0, $v0, %lo(gGameState)
801EB1A0: sw    $v1, 0x0($v0)          ; gGameState = 2
```

Grepping the disassembly for `sw ... gGameState` finds nothing, and is quietly
misleading.

Note the `%hi`/`%lo` trap in that pair. `%lo` is `0xAB24`, whose top bit is set,
so `addiu` sign-extends it: the address is `0x800E0000 - 0x54DC = 0x800DAB24`.
Concatenating the two halves gives `0x800EAB24` -- a real, writable, entirely
unrelated address. Watching that address produced several turns of elaborate
theory about a state machine that was never stuck.

### Jump tables

| Table | Entries | Indexed by | Used by |
|---|---:|---|---|
| `jtbl_800EAFA8` | 104 | `gGameState` | `func_80092CF0`, renderer dispatch |
| `jtbl_800EB150` | 72 | `gGameState - 1` | `unk_game_load` |
| `jtbl_800EB320` | -- | `gGameState - 1`, checked unsigned against `0x66` | `GameLoad_LoadOverlay` |

`unk_game_load` is a 630-instruction dispatcher that stages what the next state
needs -- course data, asset pointers. **It returns a display list pointer, not a
boolean**, even though the caller's `beqz $v0` reads like a "was a load
requested?" test:

```
80047290: jal  unk_game_load
8004729C: beqz $v0, .L800472AC     ; NOT a load-requested test
800472A4: jal  GameLoad_LoadOverlay
```

State 0 loads no overlay by design, because `state - 1` fails the unsigned bound.

`func_80093104` is the **Controller Pak check**, and it is the branch that
advances the state at boot. It tests, in order: an enable flag, the controller's
status bit, whether START is held (`andi $t2, 0x1000`), then `osPfsIsPlug`,
`osPfsInit`, `osPfsNumFiles`, `osPfsFileState` (sixteen times) and
`osPfsFreeBlocks`. It returns 1 only when a Pak is present and readable, and 0 at
the first sign it is not. With no Pak -- which is what a port that answers the Pak
API with `PFS_ERR_NOPACK` produces -- it returns 0, the caller's `beqz $v0`
branches to `func_801EB180`, and `gGameState` becomes 2. That is exactly what
hardware does with an empty controller slot, so those stubs are correct rather
than a workaround.

---

## 5. Overlay loading

Two different paths bring code into RAM, and they do not share a bottom half.

**`GameLoad_LoadCodeseg`, `SysMain_Thread` and five other sites** call
`game_dma_copy(rom, ram, size)`. Its `ram` argument (`$a1`) is a **physical**
address: the original passes it through `osPhysicalToVirtual` before the
transfer. The function is asynchronous in shape -- it starts a PI transfer, then
waits on a message queue for completion.

**`GameLoad_LoadOverlay` does not use `game_dma_copy`.** It reads an entry from
`gOverlayTable` and calls `osPiStartDma` directly, with a **hardcoded destination
of `0x802C5800`**.

`gOverlayTable` entries are eight words:

```
{ romStart, romEnd, textStart, textEnd, dataStart, dataEnd, bssStart, bssEnd }
```

So the only point both paths pass through is `osPiStartDma`. A port that needs to
know when code arrives should hook there, not at `game_dma_copy`; this was
established by tracing every transfer through `game_dma_copy` and watching
nothing ever land at `0x802C5800`.

A boot trace, useful as a reference for what to expect and in what order:

```
#1 rom 0x0A95D0 -> ram 0x801DAFA0 size 0x4CAC0   codeseg
#2 rom 0x0F6090 -> ram 0x80228E10 size 0x8290
#4 rom 0x374100 -> ram 0x802A0000 size 0x2800    asset chunks
   rom 0x7AE8B0 -> ram 0x80153978               MusicData
   rom 0x40B530 -> ram 0x80153978               SoundDataADSR
   rom 0x428C30 -> ram 0x80003240               SoundDataRaw
   rom 0x7C4B70 -> ram 0x80003390               BankSetsData
```

The compressed data region begins at ROM `0x1AE420` in v1.0, where the first of
131 MIO0 blocks sits; Rev A declares its first MIO0-bearing segment at `0x1AE660`.

---

## 6. Graphics

The microcode is **Fast3D**, in the pre-F3DEX2 opcode encoding: `G_MTX` `0x01`,
`G_MOVEMEM` `0x03`, `G_VTX` `0x04`, `G_DL` `0x06`, `G_ENDDL` `0xB8`, `G_MOVEWORD`
`0xBC`, `G_POPMTX` `0xBD`. RT64 identifies it from the OSTask's ucode text and
data addresses. `gDisplayListHead` is at **`0x80151944`**.

### How a frame is built

The game builds **one top-level display list per frame**, with every matrix load
inline in it, and calls its model and HUD lists from that list. Those called
lists are static data in the cartridge and are called by the same segmented
address every frame, which makes their addresses stable identities to key on --
useful for classifying or tagging individual elements without touching game code.

Per frame: `SysMain_GfxInitBuffers`, then the drawing, then
`SysMain_GfxFullSync`, then `SysMain_CreateGfxTask`.

### The drawn region

The game **does not draw to its whole 320x240 framebuffer.** Every frame -- title,
attract, menus, racing -- is drawn inside an inset scissor:

| | Scissor | Size |
|---|---|---|
| one player | `(8, 20)`-`(311, 219)` | 303x199 |
| two players | `(8, 12)`-`(311, 229)` | 303x217 |

with black borders around it that a CRT's overscan was meant to hide. Three
consequences for a PC port:

- 303x199 is an aspect ratio of 1.52, **14% off 4:3**. A renderer that identifies
  "the game's main 4:3 framebuffer" by comparing the scissor's shape to 4:3
  within some tolerance (RT64 uses 10%) rejects this game's frame, and whatever
  that test gates goes wrong -- in RT64's case the 2D layer was stretched across
  the widened frame instead of being kept at its original shape in the middle.
- Presenting the whole framebuffer shows the borders as black bars, and a
  widescreen multiplier turns the 8-pixel side borders into wide ones. Presenting
  the drawn region instead requires reading the region from the scissor at run
  time, because it changes with the number of players.
- A 3D pass that covers the drawn region but not the whole framebuffer trips
  "does this pass reach both edges?" tests. In the championship's **warm-up
  round** something else in the frame touches the full 320x240 buffer, so the
  frame's scissor becomes the whole thing, the world falls 8 pixels short at each
  end, and that one race is rendered at 4:3 while every other is widened.

### 2D versus 3D, and how to tell a race from a menu

The game draws **its 2D -- HUD, menus, fades -- as triangles under an orthographic
projection**, not only as rectangles. Rectangles do appear (the race HUD, the
full-screen overlays) and come in all three forms: `0xE4` TEXRECT, `0xE5`
TEXRECTFLIP and `0xF6` FILLRECT. All three carry their corners in the same
fields, and the fill rectangle is the one that gets forgotten -- the pause
screen's dim is a fill rectangle, so any classifier that only handles textured
rectangles silently skips it.

Distinguishing a race frame from a menu frame **cannot be done from `gGameState`**
(§4). It can be read from the frame itself:

- A race draws its world first, under a perspective projection, into the inset
  scissor, and then **switches to an orthographic projection to draw its HUD.**
- A menu with a 3D backdrop (title, main menu, the select screens) also draws
  world-first into an inset scissor, but **lays out its frames and labels with
  the backdrop's perspective projection still loaded.**

Measured across the title, the main menu, the select screens and a race: every
race HUD rectangle is drawn under an orthographic projection and every menu
rectangle under a perspective one, with no overlap. So "world drawn first" plus
"inset scissor" identifies a race frame, and the projection type per draw
separates HUD from menu layout.

### The world's frustum

Read out of the projection a race loads, on Dolphin Park:

| | |
|---|---|
| Near plane | 10 |
| Far plane | 16191.8 |
| Vertical field of view | 45 degrees (`m[1][1]` = 2.4142, which is cot(22.5)) |
| Horizontal | `m[0][0]` = 1.811, i.e. `m[1][1]` divided by 4/3 |

The projection is built the way `gluPerspective` does, so the planes come back
out of it as

    near = m[3][2] / (m[2][2] - 1)        far = m[3][2] / (m[2][2] + 1)

**The far plane is not what limits the view.** It sits roughly twenty times
further out than anything the game draws: quartering it to 4048 changes nothing
on screen, and only at 810 units does the scene begin to clip -- and then the
sky, the pier and the far islands all vanish at once. What limits the view is
the geometry the game builds: the water lattice is carried around the camera
(below) and the courses are not large. A port cannot buy draw distance by moving
this plane.

A race frame loads a second perspective projection with `m[1][1]` = 0.577 -- a
much wider frustum, for the sky -- so anything done to the field of view has to
be done to both or they come apart.

### The buoys, and where they stop being drawn

A buoy is **two triangles**, drawn as two calls to a pair of static display lists
in the course segment, with a modelview matrix per instance taken from a table in
segment 5:

| | |
|---|---|
| Display lists | `0x0102CD78` (first vertex `(0, 35, 0)`) and `0x0102CD90` (`(0, -35, 0)`) |
| Geometry | three vertices each, `(0, ±35, 0)`, `(-35, 0, 0)`, `(35, 0, 0)` -- a billboard quad in two halves |
| Matrices | segment 5, offset `0xA1C0`, one 64-byte `Mtx` per instance |

**The game repacks that matrix table every frame.** Whatever it decides to draw
goes into slots `0`, `1`, `2` … with no gaps: across five frames of a race the
slots used were `0-22`, `0-11`, `0-13`, `0-14` and `0-1`. So the number of
matrices written is the number of buoys the game chose, and the choice is made
before anything reaches the display list — a renderer cannot put back what was
never submitted.

**A buoy's identity is its place, and its place is exact.** The matrix a slot
holds is written from the buoy's course position every frame, so its X and Z
translation are bit-for-bit the same in every frame it is drawn --
`(4200.0, 0.0, 4800.0)`, `(3900.0, 0.0, 5200.0)` -- and buoys along a straight
stretch stand 400 units apart. The same holds for the gate markers and the
stunt rings, drawn the same way (one plain matrix load, one call to a static
list, both after the top level has pushed once, so at modelview depth 1).
Since the slot changes and the address with it, the position is the only
thing that names a buoy from one frame to the next, and it can be compared as
bits rather than rounded.

**The choice is a distance cull at about 4,500 world units.** Measured over a
race by locating the camera from the water lattice, which is built around it
(below), and taking the distance to each drawn buoy:

| Camera | Buoys drawn | Nearest | Furthest |
|---|---:|---:|---:|
| (4358, -720) | 23 | 3283 | **4501** |
| (3494, -4101) | 12 | 2335 | **4096** |
| (-3898, -4711) | 14 | 1586 | **4197** |
| (-6299, 2217) | 15 | 1026 | **4212** |
| (-282, 3880) | 2 | 4446 | **4570** |

The nearest varies with where the craft is; **the furthest is pinned in every
frame** in a band a few hundred units wide, which is the buoy spacing (about 340)
landing wherever it falls below the threshold. The count varying between 12 and
23 is the course curving away, not a cap: a cap would hold the count constant and
let the furthest reach further, and it does neither.

For contrast, the far plane is at 16,192 (*The world's frustum*, above). Buoys
stop being drawn at somewhere under a third of the distance the frustum reaches,
which is what a player sees as pop-in.

**The check is in `func_8006E674`**, which builds the list of buoys to draw:

```
0x8006EC30  mul.s   $f10, $f20, $f20     ; dx^2, the camera at $s6+0x4C..0x54
0x8006EC3C  mul.s   $f6,  $f22, $f22     ; dz^2
0x8006EC44  cvt.s.w $f14, $f4            ; (float) the limit, an int in $s2
0x8006EC48  add.s   $f12, $f10, $f6
0x8006EC4C  jal     0x800C7010           ; sqrtf -> $f0
0x8006EC60  c.lt.s  $f0, $f14            ; distance < limit ?
0x8006EC70  bc1fl   L_8006ED20           ; no -> skip this buoy
```

`$s2` is `lw 0xA4($v0)` at `0x8006E9AC`, and `$v0` is the word at **`0x801C0C80`**
-- a pointer the game leaves there to the course's environment struct. **The
pointer does not move between courses**: in a one-player race it reads
`0x801CB058` on every one, because the game copies each course's values into the
struct rather than indexing an array, so a diagnostic that waits for the pointer
to change to know the course changed fires once and never again. It does move
between *views*: two players have a copy each, and the game repoints it before
drawing each (*Each view has its own copy of the course's environment*, below).
So the limit is

| | |
|---|---|
| Pointer to the struct | `0x801C0C80`, reading `0x801CB058` for view 0 |
| The distance | struct `+0xA4`, an `int`, **5000** on courses 0 and 1, **6000** on course 2 |
| Neighbours | `+0xA0` reads 400 and `+0xA8` reads 135; both are compared elsewhere in the same function |

**It is not the buoys' limit. It is the course's.** Calling this "the buoy
distance" was reading one caller and assuming it was the only one. A census of
every display-list call over 5,000 race frames on two courses, repeated with
this single field doubled, settles what it governs: **49 of the 57 static kinds
measured on both runs moved out with it** -- buoys, gate markers, shoreline
props and scenery alike, on both courses. One integer caps nearly all of a
course's static geometry.

The census recovers the number from the drawing alone, without being told it
exists: the 99.5th percentile of the distances the buoys are drawn at is 4,997
on course 1, where the field reads 5000, and 5,969 on course 2, where it reads
6000. See [RENDER-DISTANCE-CENSUS.md](RENDER-DISTANCE-CENSUS.md) for the method.

**A second class sits at exactly twice it.** On course 1, with the field at
5000, a family of larger structures (`0x01018A38`-`0x01018D58` in segment 1)
reaches 9,916 and is never drawn beyond it, although its sites get 13,175 from
the camera. Doubling the field to 10000 lifts that class past anything the
course contains, which is what a limit of 20,000 does. So the same field governs
both tiers, one at `x1` and one at `x2`.

**The cull runs when the list is built, not when it is drawn.** A buoy accepted
at 4,999 is still in the list a frame or two later once the camera has moved off,
so a thin tail always lands past the limit: 23,526 buoy draws top out at 5,464
against 5,000, a 9% overshoot. It is the same limit measured a frame late, not
slack in it.

**One thing it does not reach.** On course 2, display list `0x0102CE78` with
texture `0x01015220` -- 746 sites -- is culled at about 5,100 and did not move
when `+0xA4` doubled: 5,108 at `x1` against 5,083 at `x2`. Its limit is
somewhere else and has not been found.

**The limit is per course**, and it varies more than enough to matter. Read out
of the struct while racing: **5000** on courses 0 and 1 and **6000** on course 2;
**3072** and **2500** were also seen in an earlier session. A course that starts
at 2500 is at half another's draw distance before anything is changed, so a
*multiplier* over it does visibly different amounts of work on different courses
-- two times on a 2500 course only reaches what a 5000 course has by default.
That is why this port's setting asks for a distance instead, and why anyone
judging one by eye needs to know which course they are on.

It is also why a port must not decide it has already done its work by comparing
the field against what it wrote there: 2500 doubled is 5000, which is another
course's own limit, so that test silently passes on the wrong course. The course
number at `0x800D8170` is the identity, and it changes **before** the struct is
filled in -- so a reader has to wait, or it reads the previous course's
environment under the new course's name.

**The buoys' display lists are not per course.** This section once said
`0x0102CD78`/`0x0102CD90` were one course's buoys and `0x0102CC58`/`0x0102CC70`
another's. Reading `func_8006E674` whole says otherwise: the first pair draws the
small buoys and the second the racing buoys, on every course, and each has a
mirrored pair (`0x0102CE08`/`0x0102CE20`, `0x0102CCE8`/`0x0102CD00`) used when the
camera's heading puts the sprite past the tenth of its eighteen angles. The
matrix offsets that looked per course -- `0x4140`, `0x9600` -- are other objects'
tables in the same segment 5 buffer; the buoys' are at fixed offsets (below). The
limit at `+0xA4` is still the right thing to key on.

Raising `+0xA4` lets the game submit the buoys it was skipping, and matrices,
display list and renderer follow -- **up to 32 of them a view**, which is the
next limit (below), and which the port removes at Extended. At four times, 5000 to 20000, the draws counted went from
12-23 to 40-54 and the furthest from 4,096-4,570 to 7,062-8,617. Those counts
predate knowing the buoys have 32 slots, so they cannot all have been buoys with
a slot of their own; read them as draws, not buoys.

### The buoys have a slot limit, and it is the buffer's

`func_8006E674` is called once per view and draws two tables of buoys, each in
two passes: mark the visible ones, then give each visible one a matrix slot and
draw it through that slot.

| | Table A | Table B |
|---|---|---|
| Records | `0x801BB138`, `0x18` bytes (x `+0`, z `+8`, type `+0x10`), count `0x801BC938`, room for 256 | `0x801AEE20`, `0x104` bytes, count `0x801BB120`, room for 64 |
| What | the small buoys: the purple course-edge rows (type 1), plus clusters that depend on difficulty (types 2, 3) | the racing buoys and gates, chained buoy to buoy; the yellow arrow is drawn above the next one |
| Built | once, at course load, by `func_80071E70` from three per-course lists appended in order; never repacked | at course load |
| Visible if | `sqrt(dx^2 + dz^2) < +0xA4`, and within 60 degrees of the camera's heading (`dot > 0.5f`, `c.lt.s` at `0x8006ECA4`; the heading is `gCameraPerspective[i]` `+0x64`/`+0x6C`, x and z, at `0x80227C80 + i*0x10C`, `i` from `0x80223930`); types 4 and 5 never | the same distance, doubled for gates; the same cone (`0x8006EAF4`), which gates and some types skip |
| Visibility marks | `int16` at `0x801C08C0`, `-1` hidden, else a fade | `int16` at `0x801C0840` |
| Slots a view | **32**, `slti $t5, 0x20` at `0x8006F01C` | **12**, `slti $t5, 0xC` at `0x80070108` |
| Slot bytes | `0x801C0B80` | `0x801C0B40` |
| Matrices, segment 5 | `0xA1C0 + view<<11 + slot<<6` | `0x95C0` (and `0x9BC0` for a second matrix) `+ view*0x300 + slot<<6` |
| Draw loops | six, all adding `slot<<6` to one base in `$s2` | eight, over three bases |
| Display list | 8 commands a buoy | 8 a buoy, 7 more for the next buoy's arrow |

**Slots go out in table order and the loop stops at the cap.** A buoy past it
keeps its visible mark and the draw loops still draw it, reading its slot byte
from an earlier frame: on top of another buoy or where it used to be. The caps
are the buffer being full, not a choice: segment 5 is `0xB2F0` bytes (the game
alternates two, `func_8006A264`), table B's regions run straight into table A's
at `0xA1C0`, and table A's two views end at `0xB1C0` where the buffer's own data
resumes.

**Where it shows.** At the game's own distance the caps are rarely reached. At a
raised one table A is over its cap everywhere, and worst at the end of a lap:
the end of the edge row, its start past the line and the difficulty clusters at
the top of the table are all in view, and the nearest buoys come last in the
table, so they are the ones without a slot. Measured with `WR64_BUOY_TRACE` over
600 views of a one-player race at 16,192:

| | Game's own | Every buoy drawn (`patches/buoys.cpp`) |
|---|---:|---:|
| Most of table A in view at once | 120 | 120 |
| Buoys left without a proper slot | 10,455 | **0** |
| Display-list commands a whole frame used | 1,723 of 3,072 | 1,723 of 3,072 |

Two-player, every buoy drawn: none left out, at most 1,748 commands a frame, and
up to 12 of table B in view at once -- exactly its cap.

Only `func_8006E674` reads the slot bytes and these matrices, and only it and the
arrows (`func_8006B334`, table B's marks) read the visibility marks; collision,
the missed-buoy count and the course logic read the records. So the port can move
the matrices without touching any of that: see *Every buoy in view* in
`patches/buoys.cpp`.

### Each view has its own copy of the course's environment

The struct whose `+0xA4` is the draw distance is not one struct. It is `0x110`
bytes a view from `0x801CB058`, and `func_8006E674` points `0x801C0C80` at the
view it is about to draw (`sw` at `0x8006E7EC`: `0x801CB058 + view * 0x110`). A
one-player race uses the first copy; two players use both. Reading the pointer at
the end of a frame, after both views are drawn, finds only the second view's --
which is how a raised distance once reached the second player's screen and not
the first's.

Two things that look like the source and are not. `0x458CA000` is exactly
`4500.0f` and occurs several times around `0x800E63C4`, but in context it is a
coordinate inside seven-word placement records. And the static table at
`0x800D8578` -- twenty-seven four-word records, each ending in the sentinel
`0x2D2D2D00`, holding distances of 5000, 3000 and 1000 on one set of courses and
6000, 4000 and 2000 on the other -- looks exactly like where 5000 comes from.
Scaling every record in it changes nothing: the value read at `+0xA4` stays 5000.
Both were checked and both are coincidences.

### The animated water: how far it reaches, and who builds it

The waves are a **fixed patch carried with the camera**, not a surface covering
the course. Measured over five frames of a race, twenty seconds apart:

| | |
|---|---|
| Vertex blocks | **50**, every frame |
| Vertices | **500**, every frame |
| Extent | 1,536 by 1,330 world units |
| Reach from its centre | **922 units**, identical in every frame |
| Grid | x steps of 32; z steps of 55 to 56 |
| Vertex arena | segment 3, `0x13D68` to `0x15AA8` -- the same addresses in every frame |

For scale, the course's static geometry is culled at 5,000 or 6,000 depending on
the course -- buoys and scenery share the one limit, see *The buoys, and where
they stop being drawn* above -- so the animated water is a patch roughly a sixth
of the course across.

**There is no water beyond it.** This document used to say the sea past the patch
was "drawn some other way, which is why the sea still meets the horizon", and
that was a guess standing in for a measurement. A whole-frame trace of a race
says otherwise: across the entire frame, the only geometry within two units of
water level is the patch's own 50 segment-3 blocks and a handful of segment-8
loads that are the craft. Nothing else is at sea level at all.

What fills the screen from the patch's edge down to the bottom is **the lowest of
the sky's three bands** -- the haze band at `0x06000000`, seven vertices, drawn
from segment 6 under the identity model matrix like the rest of the sky. Its
first vertex sits at **y = 0**, water level, and in the traced frame it spans
clip y **-0.63 to 0.15** while the patch's 50 blocks span **-8.87 to 0.15**: the
two meet at exactly the same line, and the band carries on below the patch to the
bottom of the screen.

So the distant sea is **painted on the sky**, and the patch is drawn over it. That
is the whole trick, and it is why the water looks like it reaches the horizon
when it stops at 922 units. (The band's outer vertices are inferred from its first
vertex and its screen extent; the trace records only v0 per load.)

It also sets the limit on what a port can do here. There is no flat far-sea
geometry to shade or to replace -- anything that makes the sea reach further has
to put geometry where the game draws none, over a band the game painted to look
like sea.

**The builder is `func_80050204`**, and the call that fills the arena above is at
`0x80051200`:

```
0x800511F0  lw    $a1, 0x18B8($a1)     ; the segment 3 base, from 0x801518B8
0x800511F4  lui   $at, 0x1
0x800511F8  ori   $at, $at, 0x3D68     ; 0x00013D68, the arena offset
0x800511FC  or    $a0, $zero, $zero    ; 0
0x80051200  jal   0x80050204
0x80051204  addu  $a1, $a1, $at
```

A second variant at `0x80051218` uses offset `0x170D8` instead, chosen by a test
on `0x80192420` just above -- most likely the split-screen case, which draws into
a taller region.

**The vertex budget is what makes this hard.** Five hundred vertices is not a
number a port can raise by changing a comparison: it is the size of an arena, and
the RSP has its own limit on a vertex load. Reaching further with the same budget
means wider spacing -- more area, coarser waves, and waves the wrong size for the
craft riding them.

**And the spacing is computed, not stored.** Instrumenting the multiply that
places each vertex -- `0x8005042C`, `mul.s $f10, $f8, $f2` -- gives, in a race:

| | |
|---|---|
| `$f2` | **0.2787**, the scale |
| `$f8`, `$f4` | 192 and 220, the grid coordinates it multiplies |

That 0.2787 was then looked for three ways and found none of them. Deriving its
address from the `lwc1 $f2, 0x200($v0)` a few hundred instructions later gives
`0x80227E80`, which reads zero for a whole race. Scanning RDRAM for the grid's
apparent steps -- 32 and 55 -- finds no adjacent pair anywhere. Scanning for
0.2787 itself finds only `0x801545C0` and `0x80156000`, which turn out to be two
**mirrored linear ramps** from 0.2266 to 0.3297 in steps of 0.00147 that merely
contain neighbouring values; 0.2787 is not in either.

So unlike the buoys' limit, the *spacing* is not a number sitting in memory
waiting to be scaled. It is derived per frame, and changing it means intervening
inside `func_80050204` -- 1,458 lines of recompiled C -- rather than writing a
word.

**That conclusion was right about the spacing and wrong about the water.** The
heights are not generated per frame either: they come out of a world-fixed
wrapping field in RDRAM that covers thousands of units and can be read at any
point, which the next section documents. What the 500 vertices limit is how much
of that field gets *drawn*, not how much of it exists.

### The wave field: where the water actually comes from

**The water is not procedural and it is not a surface built per course. It is a
world-fixed, wrapping, triangular lattice of 64-unit cells held in RDRAM**, and
everything else -- the drawn mesh, the buoyancy, the craft contact -- reads from
it.

| | |
|---|---|
| Array | `D_80162420`, at `0x80162420` |
| Shape | **384 rows x 128 columns**, entries of `{ s16 height, s16 age }` |
| Size | 196,608 bytes, so it ends at exactly `0x80192420` |
| Cell | 64 world units, on a **triangular** (oblique) lattice |
| Height | the s16 at entry offset 0, used as **`s16 >> 8`** -- whole world units, floored |
| Wrap | `% 24576` on both axial coordinates, which is 384 x 64 |

That the array ends exactly where `func_80050204`'s split-screen test reads
(`0x80192420`), with `gWaterLevel` immediately after at `0x80192458`, is the
first confirmation the shape is right.

**The indexing**, read out of `func_8004D30C` at `0x8004D30C`-`0x8004D3FC` and
matching the decompiled `func_8004F3D4` exactly:

```c
v   = (s32)(z * 1.1547005f) % 24576;        // 2/sqrt(3), from z alone
u   = (s32)(z * 0.57735026f + x) % 24576;   // 1/sqrt(3), sheared by x
cv  = v >> 6;   cu = u >> 6;                // arithmetic shifts
row = (cv + (cu & ~0x7F) + 0x600) % 384;
col =  cu & 0x7F;
height = (s16)field[row * 128 + col] >> 8;
```

Three things in that are worth pausing on.

**The row absorbs the high bits of `u`.** The array is only 128 columns wide, so
`(cu & ~0x7F)` folds every 128 columns of `u` into the row index. A sheared 2D
space is packed into 384x128 by that fold, which is why the row depends on the
*column* coordinate at all.

**`+ 0x600` is 4 x 384**, a bias to keep the sum positive before the modulo. C's
`%` keeps the sign of the dividend, so without it a negative world coordinate
would index behind the array.

**The height is shifted, not scaled.** `func_8004F3D4` writes `arg2 * 256` into
the s16, so the stored unit is 1/256 -- but every reader takes `s16 >> 8`, which
floors to a whole world unit and discards the fraction. Reading it as `h/256.0`
looks almost right and is wrong by the discarded fraction; measured against the
game's own query, that mistake cost a **median error of exactly 0.4950 world
units**, which is the mean of a uniformly distributed discarded fraction. It is
the kind of error that looks like an imperfect decode rather than a wrong one.

**Which cell, of the two:** `func_8004D30C` compares the two fractional parts
(`0x8004D410`-`0x8004D424`) and branches on `frac v > frac u`, which is the
diagonal `frac u == frac v` splitting each rhombus into two triangles.
`func_8004F3D4`'s two candidate cells -- `(cu, cv+1)` and `(cu+1, cv)` -- are
the off-diagonal corner of each half, which is the same split seen from the
writing side.

Each half then forms a plane from three corners, and what the code computes is
a pair of **differences** between adjacent corner heights:

| Branch | Corners | Differences |
|---|---|---|
| `frac v <= frac u` | `(cu,cv)`, `(cu+1,cv)`, `(cu+1,cv+1)` | `h00-h10` at `0x8004D548`, `h10-h11` at `0x8004D584` |
| `frac v >  frac u` | `(cu,cv)`, `(cu,cv+1)`, `(cu+1,cv+1)` | `h00-h01` at `0x8004D470`, `h01-h11` at `0x8004D4D8` |

They are combined in 12-bit fixed point at `0x8004D588`-`0x8004D600`:
`(gWaterLevel + h00) << 12`, plus each difference times a saved weight, with a
square root of `d0^2 + d1^2 + 0x1000` taken at `0x8004D5C4` -- a plane normal
being normalised, which the function also gives its callers for slope.

**The weights are `-(u & 63)` and `-(v & 63)`, and the residual was never the
weights.** Both are formed at `0x8004D414`; the whole tail from `0x8004D588`
transcribes to

    return (H << 12  -  d0*fu  -  d1*fv) / sqrt((d0^2 + d1^2 + 0x1000) << 12)

with `H = gWaterLevel + h00` and `fu`, `fv` the offsets inside the cell in units
of 0..63. `0x1000` is 4096, which is 64 squared, so the divisor is
`64 * sqrt(d0^2 + d1^2 + 64^2)` -- **the length of the normal of a plane rising
d0 and d1 over a 64-unit cell**.

So `func_8004D30C` does not return a height. It returns that plane's constant
divided by its normal's length, which is the other half of the same computation
as the slope it hands its callers. On flat water `d0 = d1 = 0` and the divisor
cancels to exactly `H`, which is why every check made on still water agreed with
a plain plane and the division went unnoticed for so long. As the surface tilts
the two diverge, and that divergence *is* the "about one world unit" this
document used to attribute to unknown weights.

With the division in place the decode reproduces the game **exactly**: over
19,042 probes taken inside cells, 100% within tolerance, median error 0.0002 and
0.0000 on the two interior grids.

**A caller that wants a height multiplies back** by `sqrt(D0^2 + D1^2 + 1)`,
where `D0` and `D1` are the differences over the cell size. The port does not --
`src/waterfield.cpp` and `src/waterring.cpp` both take the return as a height --
which makes their heights low by that factor: about 0.6% on typical slopes and
up to 15% on the steepest. For the water ring, which is far-field geometry, that
is invisible; for anything that needs the real surface it is not.

**The third probe grid was never testing what it claimed.** The one named
`lattice`, meant to sample cell corners, puts **100%** of its probes on a cell
boundary -- 2,205 of 2,401 at `(63,63)`, one unit short, because the float
round-trip from lattice coordinates back to world XZ truncates into the previous
cell and the decoder and the game then need not agree which cell a boundary point
is in. It is a broken measurement, not a broken decode, and it is the second time
that grid has cost this project an investigation, so `tools/decode_water_field.py`
now prints each grid's boundary share and excludes a boundary grid from its
verdict.

#### How this was checked, and what it is for

`WR64_WATER_FIELD=<prefix>` dumps the array and, in the same frame, probes
**the game's own height query** -- `func_8004D30C(x, z)`, called through the
recompiled code from the graphics-task hook -- on a grid of thousands of points.
`tools/decode_water_field.py` then decodes the array and compares. Reading the
code gives a model; reproducing the game's own answers is what makes the model
something geometry can be built from.

Where it stands: on lattice-aligned probes the decode agrees to a median of
**0.089 world units**, against a field spanning about 90. Probes placed inside
cells still differ by about a unit, which is the interpolation weights that have
not been transcribed yet.

One trap worth recording. Probing "on a cell corner" is the hardest place to
test a truncating index: computing the world position of a corner in floats and
handing it back produces `u = 1535.9999`, which truncates into the neighbouring
cell. The first round of this measurement placed every probe on a boundary and
produced small errors everywhere, which read as a broken decode and was a broken
measurement. Probe cell *interiors* and compare, or accept that the boundary
cases will disagree.

**Why this matters for draw distance.** The field is roughly 8,192 world units
across in `u` before it folds, and `func_8004D30C` will answer for any point in
it. The game *draws* 500 vertices reaching 922 units. The surface data for a
much longer view already exists -- what is limited is the drawing, not the
simulation, so a renderer can extend the water without inventing waves and
without disagreeing with the physics.

#### Does index *i* keep its meaning? Measured per vertex

The per-block summaries above say the lattice carries rigidly, and concluded
from that that index *i* keeps its meaning. The fork this port's water renderer
came from concluded the opposite -- that pairing by index blends unrelated water
-- and built world-XZ resampling on it. `WR64_LATTICE_VERTS` and
`tools/lattice_shift.py` settle it with every vertex rather than each block's
first, and **the fork is right**.

The test: if heights travel with the lattice, vertex *i* keeps its height across
a carry. If they are resampled from the world-fixed field, they shift along the
index by however far the lattice moved. Two details decide whether the test says
anything at all.

**The search has to be two-dimensional.** A block is a row of the lattice and
the index is a column, so a search over the flattened vertex order finds a
column shift and is blind to a row shift -- a carry of one column *and* one row
reads as no shift.

**Only some frames can answer.** An index shift can express the carry only when
the lattice moved a whole number of columns along x and did not move in z. A
mixed carry of 32 in x and 55 in z is not a whole number of spacings in either
axis, so no shift fits and that frame says nothing either way. On the frames
that do qualify, compare the error at the best shift against the error at no
shift:

| Course | Frames | Qualifying | Error, no shift | Error, best shift | |
|---|---:|---:|---:|---:|---|
| Dolphin Park | 120 | **69** | 3.204 | 2.301 | **28% lower** |
| course 1 | 16 | 4 | 2.954 | 1.784 | 40% lower |
| course 2 | 121 | 2 | 3.500 | 1.722 | 51% lower |

**Shifting the index reduces the error substantially, on every course.** The
heights are attached to world position, not to the index: the lattice carries
its *positions* rigidly, but the heights it samples come from the world-fixed
field underneath it, so vertex *i* is a different piece of water after a carry.
Pairing by index blends water from two places, and a renderer that interpolates
per index interpolates between unrelated samples.

It only *looks* harmless because most frames carry diagonally, where no integer
shift fits and the comparison cannot separate the two accounts. The residual at
the best shift is also small in absolute terms -- 3 to 7% of the height spread
-- but that is a property of the field being smooth, not of the pairing being
right.

**A correction worth recording.** The first capture of this was sixteen frames
on one course, of which exactly **one** qualified, and that frame showed a 9%
improvement -- noise. It was written up as confirming the rigid-carry
conclusion. Repeating on another course produced the opposite verdict, and
capturing 120 frames instead of 16 turned the original course's single frame
into 69 and reversed it too. The lesson is in the shape of the data, not the
subject: when a test only applies to some frames, the number that matters is how
many qualified, and a verdict from one of them is not a verdict. The tool now
refuses to conclude from fewer than three.

### Stunt mode's rings

Stunt mode runs in `gGameState` **`0x28`**, the same state as a race, so a
diagnostic keyed on race states reaches it without being told about it.

| | |
|---|---|
| Display list | `0x0101E620`, called through `0x0101EA48` |
| Geometry | 48 triangles -- a real torus, not a billboard |
| Matrices | segment 5, a table stepping `0x40`, seen from `0x4200` to at least `0x4700` |
| Positions seen | 22 in one run |

**No distance cull was found.** Over thirteen traced frames the number drawn
varied between 2 and 10, and the furthest drawn ranged from 2,507 to **9,406**
with no ceiling -- against the buoys, whose furthest sat between 4,096 and 4,570
in every frame, which is what a distance limit looks like. 9,406 is beyond the
distance at which buoys are dropped entirely.

So what looks like rings popping in is most likely them entering the view rather
than passing a threshold. It is worth saying that this is a **negative result
from one run**, not a proof: a ring that is never drawn anywhere in the trace
cannot be distinguished from one that does not exist, so a cull that hides
everything past some distance would leave no trace of the things it hid. What
would settle it is catching one ring appearing and knowing roughly how far away
it was.

### A two-player frame

Measured from RT64's camera pairing in a 2P VS start on Sunny Beach, with the
pairing log's camera lines (`WR64_PAIRING_LOG`):

| Camera ("scene") | Framebuffer slot | Screen region, game pixels |
|---|---:|---|
| Top view | 1 | (8,12)-(311,120) |
| Bottom view | 1 | (8,122)-(311,229) |
| Both views at once, when their cameras coincide | 1 | (8,12)-(311,229) |
| A perspective projection that draws nothing | 0 | none |

- **The two views are separate cameras on the same framebuffer**, one above the
  other. A renderer tells them apart by matrices alone unless told otherwise.
- **Slot 0 carries an empty perspective scene in every race frame**, one player
  or two. It draws nothing, but a renderer that pairs cameras by matrix alone
  will pair it with a real view.
- **The intro camera is continuous.** From the fade-in (~34.2 s after launch with
  `tools/scripts/race-2p.txt`) through the close-ups to the start line, each
  view's camera changes smoothly -- RT64's matrix difference rises to ~320 a
  frame during the sweep and falls under 5 at the line -- with no hard cut. Both
  views film the same riders from nearby for the first second and a half, which
  is why their cameras are easy to confuse.
- **This race asks for 20 frames a second** (divider 3), measured in two runs,
  and so did the one-player race `tools/scripts/race.txt` reaches. That is not
  what the table under *Video timing* says for a race; see the note there.

### The racers: how a rider is drawn

Each racer is **one call at the top level to a list the game builds every frame
in segment 2**, and every matrix of the rider and craft is loaded *inside* that
list -- nothing about a racer is visible from the top level but the call.
Measured over eight consecutive frames of a four-racer Time Trial start, from a
3D trace (`WR64_3D_TRACE_STATE=28 WR64_3D_TRACE_FRAMES=8`):

| | |
|---|---|
| Calls per frame | 4, one per racer, e.g. `0x02000000`, `0x02000290`, `0x02000668`, `0x020008F8` |
| Matrix loads per racer | **18**, plain loads (`G_MTX_LOAD`, no push), from segment 3 |
| Matrix addresses | **`0x0300E108 + limb * 0x100 + racer * 0x40`** -- a fixed table, the same segmented addresses in both of the game's alternating lists |
| Load order within a racer | fixed: `E108, E208, ..., E708, E908, EB08, ED08, EF08, F108, E808, EA08, EC08, EE08, F008, F208` (+ `racer * 0x40`) |
| Nested calls | 18 per racer, to model parts in segment 8, vertices only -- no matrix loads |
| At the start line | the four stand 100-250 units apart |

Two things about identity follow, and both were measured rather than assumed:

- **A racer's list address is not its identity.** One racer was drawn from
  `0x02000668` for three frames and from `0x02000520` for the next three, with
  the same matrices inside. The slot a racer's list occupies in segment 2 can
  move mid-run.
- **A matrix's address is.** `0x0300E188` was the third racer's first limb in
  every frame, whichever list it was drawn from. So anything that has to pair
  a limb with itself across frames -- the renderer's interpolation is the case
  here -- should key on the matrix's segmented address, not on the list or on
  the load's ordinal within it.

**The same table on the watercraft select screen, drawn differently.** Traced in
state `0x0A` (`WR64_3D_TRACE_STATE=0A`, `tools/scripts/rider-select.txt`):

| | |
|---|---|
| Where the part matrices are loaded | **the frame's own list**, plain loads at modelview depth 0 -- no segment 2 model list |
| Addresses | the same table, `0x0300E108` + part * `0x100` |
| After each load | a call straight to the part's mesh in **segment 8**, e.g. `0x080208A8` |
| Parts | **18 or 23**, depending on the rider selected -- switching to a 23-part rider brings 5 parts that were not drawn the frame before |
| While switching | the model moves 30-50 units a frame |

The part meshes, and so their draw calls, change with the rider while the
matrix addresses do not, which makes the address the only identity that
survives a switch.

A racer's **spray** is separate: a cloud of a few dozen single-matrix draws
around the craft, each rebuilt every frame, moving 15-75 units a frame and
shuffling among themselves. They are particles, not parts, and nothing keys them.

### The gate markers: identified, and what limits them

The **yellow arrows on the course** are not culled the way the buoys are. Raising
the buoy limit at struct `+0xA4` moves the buoys and leaves the arrows exactly
where they were.

Identified by right-clicking one in RT64's debugger, reading the texture out of
`Load operation #0`, and resolving it against the segment bases in a trace of the
same session:

| | |
|---|---|
| Display list | `0x0102CE78` |
| Vertex block | `0x0102CE38`, 4 vertices at `(+/-64, +/-64, 0)` -- a billboard quad |
| Texture | **`0x01014A18`**, i.e. physical `0x0023D828` at that session's segment 1 base |
| Matrices | **segment 5 at `0x4440`**, stride `0x80` -- a fixed table, like the buoys' at `0xA1C0` |

Note how close the display list sits to the buoys' `0x0102CD78` and `0x0102CD90`:
the same neighbourhood of the same segment, which is a hint they belong to one
object system with different types. `func_8006E674` does branch on a type field at
`+0x10` of each record; types 4 and 5 are always marked hidden by it (*The buoys
have a slot limit, and it is the buffer's*, above).

**They are not culled by distance at all.** Measured over a full lap, thirty
frames spread across forty-five seconds of racing:

- **Thirteen positions** exist in the table, at matrix slots `0x4140` to `0x44C0`,
  one `Mtx` apart. Several slots share the same coordinates -- `0x4200`, `0x4280`,
  `0x42C0` and `0x4340` are all at `(-1166, 966)` -- so one signpost has several
  entries, differing in the direction the arrow points.
- **One or two are drawn per frame**, never more, never none.
- **The drawn ones are not the nearest ones.** In 27 of 29 frames the game skipped
  nearer arrows to draw further ones: at one point it drew an arrow at 7,114 while
  skipping arrows at 5,286 and 6,448, and at another it drew out to 4,923 while
  skipping three at 531.
- The furthest ever drawn was **8,746**, beyond the distance at which buoys are
  culled.

An arrow at 531 units being skipped while one at 4,923 is drawn rules out a
distance rule, and rules out a nearest-N rule with it. **The selection is by
relevance to the racing line, not by distance**: these are navigational markers
telling the player which way the course goes next, and the game draws the one or
two that apply to where the player is.

So there is nothing here for a draw distance setting to reach, and nothing being
lost to a limit. Drawing more of them would mean showing directions for parts of
the course the player is not on, which is a change to what the game says rather
than to how far it draws.

**One dead end, recorded so it is not repeated.** An earlier attempt read a
different call's matrix address, `0x0300F208`, and treated it as an identity. It
is not: segment 3 is a per-frame arena, so an offset there says only where the
allocator landed that frame on that course. Matching it against a trace of another
course produced a confident wrong answer -- a display list drawn twice at 400 to
700 units, which is the player's own craft. **Only a fixed segment, or a texture,
identifies an object between sessions.**

### Placing a 3D object inside a 2D layout

The game does not give an object its own small viewport. It takes a **full-size
viewport -- scale 160x120, the whole 320x240 frame -- and moves its centre** to
where the object should appear, then draws the object at the origin of a
square-aspect perspective projection (`[0][0] == [1][1] == 3.370880`, fovy 33
degrees, near 16, far 4096).

| Screen | `gGameState` | Viewport | Centre (x, y) | What is placed |
|---|---|---|---|---|
| Rider select | `0x0A` | `0x800DA8F0` | 226, - | the rider's craft |
| Course overview | `0x1E`, `0x1F` | `0x0106F728` | 105, - | the course preview |
| Race results | `0x34` | `0x07001280`..`0x070012B0` | 86, 88 (and three more) | one craft per finishing position |

The full-size scale is the trap. A widescreen renderer that asks "does this pass
cover the frame's width?" -- and measures a viewport through its clip ratios,
which are `3 3 -3 -3` here, so a 320-wide viewport measures 1920 wide -- answers
yes however far the viewport has been moved. So **the viewport's translate is the
only thing in the frame that says the object was placed rather than filling the
screen.** Nothing else distinguishes it: same scale, same clip ratios, same
scissor as the backdrop.

The same projection value, `3.370880`, is also what the wipe between menus uses
(below). It is the game's "draw this in screen space" projection, and it appears
on a screen-filling quad and on a placed object alike, so it does not identify
either on its own.

That measurement is about **rectangles**. The menus also draw *triangles* under
an orthographic projection, and the one that matters is the cursor: the pair of
red cubes that flank the selected entry.

| | |
|---|---|
| Display list | `0x0106F408`, called twice per frame -- once for each cube |
| Texture | in the dynamic segment (`0x08......`), so it moves between screens; the list address does not |
| Position | clip x -0.49 and +0.49, y 0.26..0.34, symmetric about the centre |
| Projection | orthographic |
| Screens | the title menu (`gGameState` `0x03`) and the mode menus (`0x04`) |

It is symmetric about the centre and it belongs to the entry between the two
halves, so **it must be centred, not anchored.** A widescreen classifier that
anchors an element to the nearer edge -- the right rule for a race HUD -- pulls
one cube to each side of the screen and leaves the entry alone in the middle.
The cubes are what a port has to name; nothing else on these menus needs it.

Full-screen 2D overlays do not sit where you would expect them to:

| Overlay | Horizontal span on the 320-wide screen |
|---|---|
| pause-screen dim (drawn as horizontal strips) | columns 32 to 336 -- offset right, and over the far edge |
| tint over a race | columns 9 to 310 |
| widest HUD element | five sixths of the drawn width |

A "covers the frame" test on width alone, at nine tenths of the drawn width,
accepts both overlays and still refuses the HUD. Testing height as well rejects
the dim, which is drawn as strips.

### The opening's full-frame rectangles

The intro composes its camera shots from rectangles covering the whole drawn
region, issued **under the world's own perspective projection** rather than an
orthographic one. Two different things are drawn that way, and a port widening
the frame has to tell them apart:

| Identity | What it is | Widescreen |
|---|---|---|
| the shot's own picture | part of the 3D pass, already drawn at the frame's full width | leave alone; stretching magnifies the picture |
| `tex:0x01005748` | the sun glare laid over the shot | stretch, or it hazes only the middle 4:3 of a widescreen frame |

Nothing in the frame separates them -- same projection, same extent (`9..310`,
`21..218`, the drawn region), both textured -- so the texture address is the
discriminator, and this port keeps it in a built-in tag table.

### The wipe between menus

Choosing a rider wipes to the course overview through state `0x0B`, which lasts
0.6 s -- too short for once-a-second screenshots, which is why it took a rectangle
log to see. The wipe is drawn under an **orthographic** projection, with the
game's **inset** scissor (`8,20`-`311,219`), as horizontal strips 5 pixels tall
in four textured tiles per row spanning `x 0..96`, `96..192`, `192..288` and
`288..384`, with rows running to `y 288`: the game overdraws its own 320x240 by
20% in both axes, and the overdraw is not centred (`0..384` centres on 192). A
port widening the frame has to get all four tiles out to its edges. This port
does since 0.8.0; anchoring the tiles to the frame's edges was not enough on its
own, and what was: [PORTING.md](PORTING.md), *The transition curtain: let RT64
widen what it already would*.

### The sky

Three bands -- haze, horizon, clouds -- of seven vertices each, drawn under the
**identity model matrix** with the camera on the projection stack, exactly as the
world is. But the sky is not part of the world: **the game rebuilds its vertices
into a scratch buffer behind segment 6 every frame**, following the camera and
scrolling the clouds.

Its own two matrix loads are inside the game's static lists, so the top-level
list's first world matrix load is the course, not the sky. Segment 6 having a
base at all is a reliable signal that the sky is about to be drawn.

Measured over the same 2,172 race frames as the water below (`WR64_LATTICE`,
segment 6), the sky behaves differently from it in every respect that matters to
a renderer pairing frames:

| | |
|---|---:|
| frames in which a band moved in X or Z | 95% |
| frames in which all three bands moved by the **same** step | 5% |
| frames whose largest step was 60 world units or more | 37% |
| blocks whose first vertex changed height, ever | 0 |

**The haze band is the distant sea.** Its first vertex sits at y = 0, water
level, and it fills the screen from the horizon down to the bottom -- everything
outside the animated patch's 922-unit reach is this band, not water. See *The
water* below, where the frame that measured it is described.

The three bands move **independently** -- each follows the camera at its own
rate, which is the parallax between haze, horizon and clouds -- and none of them
ever changes height. The block count is a constant three of seven vertices. So
unlike the water, which is one rigid mesh carried whole, the sky is three
coherent objects each moving on its own, and index *i* within a band keeps
naming the same point of that band. Pairing them by index and interpolating is
sound, and it is what smooths their motion. (The trace samples each band's first
vertex, so correspondence *within* a band is inferred from the constant block
count and the smooth per-band motion rather than measured directly.)

### The water

A lattice of rows marching away from the camera: **fifty vertex blocks reached
through segment 3**, under that same identity matrix, drawn from a single static
list. The game recomputes it every frame -- across four consecutive race frames,
41 of the 50 blocks carried different vertex data each time while the course and
the models were byte-for-byte identical.

Which list is called is decided by `Draw_WaterEffects` (`code_43DA0.c`), from the
player count, `D_801CE64C` and `D_800DAB2C`:

| List | Drawn for |
|---|---|
| `0x010082F0` | the opening and rider-selection scene (`D_801CE64C == 1`) |
| `0x0100B590` | one player |
| `0x0100D258` | two players, upper view |
| `0x0100E680` | two players, lower view |

Every block's vertex count is the same every frame: 13, 14, 15, then 16 for the
rest.

**The lattice is not fixed. It is carried with the camera, in quantized steps.**
An earlier measurement over four consecutive frames found every block's first
vertex bit-for-bit identical and concluded that the lattice stood still while
the heights on it moved. Four frames is an eighth of a second, and those four
were taken with the camera at rest. Measured over 2,172 race frames instead
(`WR64_LATTICE`, state `0x28`):

| | |
|---|---:|
| frames in which the lattice moved in X or Z | 74% |
| frames in which **all fifty blocks moved by the identical step** | **100%** |
| frames whose step was 60 world units or more | 31% |
| blocks whose first vertex changed height, on a 60-unit carry | 15.1 of 50 |
| ...on a frame where the lattice did not move at all | 12.2 of 50 |

Three things follow, and the third is the one that matters.

**It is a rigid carry, not a recentring** -- every block moves by the same step
in every frame measured, so index *i* still names the same lattice *slot*.

**That is not the same as index *i* meaning the same water**, which is what a
renderer pairs on, and it was read that way here for too long. The slot moves
through a world-fixed field, so it samples a different height after a carry. See
*Does index i keep its meaning?* below, which measures it per vertex and comes
out the other way.

**The wave pattern looks as though it is carried with it.** A 60-unit carry
changes about as many block-first heights as standing still does (15.1 against
12.2 of 50), which was read as the heights travelling with the lattice.

They do not. The field *is* world-fixed -- it is `D_80162420`, documented below
-- and the reason a carry changes so few of these heights is that they are
floored to whole world units by the `>> 8` every reader applies, so a smooth
field reads the same either side of a 64-unit step in most places. Fifty block
firsts is also a thin sample of five hundred vertices.

**The positions are quantized and the camera's motion is not.** The step is
almost always a multiple of 32 world units -- 64, 32, 96, 128 -- while the
camera moves a few units per frame. `func_8008E794`, which builds the surface,
loads `64.0f` in both of its branches next to a `trunc.w.s` whose result goes to
an integer register, which is that quantization in the game's code. So the
vertex positions are not a sampling of where the surface is; they are the
camera's position rounded. Interpolating between two of them makes the surface
surge 64 units across the frames in between, in the 31% of race frames that
carry. Interpolate the heights and texture coordinates of a mesh like this, and
leave its positions where the game put them.

### Segments

| Segment | Contents |
|---|---|
| 1, 8, 13 | course and models -- byte-for-byte identical between frames |
| 3 | geometry the game builds this frame; in a race frame the whole of it is the water surface |
| 5 | spray and splash particles, rebuilt every frame |
| 6 | the sky's scratch vertex buffer |

A segment's base is not fixed for the run: the game rewrites the table between
courses and between the screens that are not races. A segmented address names a
segment and an offset, so the same address resolves to different memory at
different times, and anything a port remembers about a list has to be remembered
against the physical address the table gave at the time.

---

## 7. Reading a race from RDRAM

What the game knows about a race in progress, for a port that wants to react to
it -- controller feedback here, but the same values would drive a speedometer, a
replay, or a telemetry overlay. All of it is read-only; none of it is a place to
write.

The addresses were identified by Elliott Tate in pull request #2 of this project
and re-verified here against a scripted race with `WR64_HAPTICS_TRACE` (see
[PORTING.md](PORTING.md), *Diagnostics*). What "verified" means: the countdown
counts 3, 2, 1 and hands over to the race, speed rises from zero to 118 under
throttle and the craft is airborne in 41% of the frames of a run driven straight
into the waves, buoys and misses advance where the HUD says they do, and the run
ends with the retirement the script earns.

**Globals.**

| Address | Contents |
|---|---|
| `0x80151960` | frame counter; a race's own tick |
| `0x800D8170` | course id, 0-8 |
| `0x800DAB24` | game state; `0x28` is a race, `0x28`-`0x2C` its surroundings |
| `0x800DAB28` | players, 1 or 2 |
| `0x801982F0` | riders on the course, 1-4 |
| `0x800D48DC` | which race slot the human holds |
| `0x801CE624` | half; `0xFFFF` while **not** paused |
| `0x801CE648` | 3 while a race is under way |
| `0x801CE650` | race phase; 2 and 3 are the race proper |
| `0x80228D08` | 1 while the start countdown is live |
| `0x80228A90` | that countdown, 60 down to 0 -- `(n - 1) / 15` is the number on screen |

**Per rider, by race slot.** Two arrays, one for the craft's simulation and one
for its standing in the race. Index both with the slot from `0x800D48DC`, and
only after the state, course, player and rider counts have been checked: outside
a race the slot is whatever the last one left behind.

| Array | Base | Stride |
|---|---|---:|
| craft | `0x80192690` | `0x1718` |
| standing | `0x801C2938` | `0x378` |

| Craft offset | Type | Contents |
|---|---|---|
| `+0x0028` | word | hull contact points tested this step |
| `+0x0B60` | word | steering, in the stick's own ±80 |
| `+0x0B7C` | float | vertical velocity, up positive |
| `+0x0B90` | float | speed; the HUD shows this ×1.8 |
| `+0x0C40` | float | craft-to-craft closing speed, worst this step |
| `+0x0C44` | float | craft-to-barrier closing speed, worst this step |
| `+0x0C54` | word | animation id (`0x17`, and 7 before frame `0x38`, are the crash) |
| `+0x0C58` | word | animation frame |
| `+0x0C66` | half | throttle; `0xA000` masked in while held |
| `+0x0C78` | word | contact points touching water -- over `+0x28`, how wet the hull is |
| `+0x0C7C` | half | zero while airborne |
| `+0x1608` | half | crashed |

| Standing offset | Type | Contents |
|---|---|---|
| `+0x0000` | word | lap; 1 from the moment the race starts |
| `+0x000C` | word | buoys passed |
| `+0x0028` | word | 1 when the last buoy was taken on the correct side |
| `+0x012C` | word | power, 0-5. The first racing frame is where the game fills the standings in -- in a championship race it read 5 there, in a run that took no buoys it stayed 0 -- so treat that frame as the baseline rather than as a change |
| `+0x0134` | word | buoys missed |
| `+0x02EC` | word | retired |
| `+0x02F4` | word | finished |

`osViSwapBuffer` is the place to read all of it: the game calls it once per frame
it has finished, so the state is complete and consistent, and a port's hook is
already on the game's own thread there.

---

## 8. Video timing

The game picks its own update rate by writing a divider that the video interrupt
handler counts retraces against: **`D_800D461C`**, read by `Main_Thread` and
written as each game state begins (`code_4C750.c` and `codeseg/B97B0.c` in the
decompilation's layout). The rate is `60 / divider`:

| Screen | Divider | Rate |
|---|---:|---:|
| boot, briefly | 1 | 60 |
| a race (championship, 2P) | 2 | 30 |
| menus, attract demo, Time Trial | 3 | 20 |

**Note, September 2026:** the port's frame-rate report measured a 2P VS race on
Sunny Beach, and the race `tools/scripts/race.txt` reaches from the championship
menu, both asking for **20** (divider 3) from the moment the race state began,
with 30 only on the course select and the results screens. The race row above
may describe a different moment of a race, or be wrong; it has not been
re-checked against the decompilation.

`osViSwapBuffer` is called exactly once per frame the game finishes, which makes
it the right place to measure the rate the game is actually achieving against the
rate it asked for. The two answer different questions: whether the port is
keeping up, and what the game chose.

---

## 9. Audio

The game mixes on the RSP, not the CPU. Its audio thread builds a list of ABI
commands each frame, hands it to the RSP as a task, and the RSP writes finished
16-bit stereo samples into RDRAM; the CPU then points the AI at that buffer with
`osAiSetNextBuffer`.

### The sequence players, and where music can be separated from effects

The CPU side is the sequence-player engine most of this era's Nintendo games
share -- the one decompilations name `gSequencePlayers`, four players of sixteen
channels each. This is the only place where music and effects are still separable:
after the RSP has mixed, nothing downstream can tell them apart.

| | |
|---|---|
| `gSequencePlayers` | `0x8003FCC8`, four players, stride `0x140` (the next symbol, `gSequenceChannels`, is `0x800401C8`) |
| player 0 | the effects, sequence id 0 |
| player 1 | the music: sequence 3 is the main theme, and the courses follow |

Offsets within a player, verified by reading them during a run:

| Offset | Type | Field |
|---|---|---|
| `+0x00` | byte | flags: `enabled` is `0x80`, `recalculateVolume` is `0x04` (MIPS packs bitfields from the top bit down) |
| `+0x04` | byte | sequence id |
| `+0x18` | float | `fadeVolume` -- the engine's own fade, ramped per frame |
| `+0x28` | float | `fadeVolumeScale` -- a scale for something outside the sequence to duck it |
| `+0x2C` | float | `appliedFadeVolume`, recomputed as `fadeVolume * fadeVolumeScale` whenever `recalculateVolume` is set, and cleared afterwards |

**`fadeVolumeScale` is the hook for a music volume**, and the game uses it too:
it was measured ducking the title music to `0.55`. Every write to it in the game
is an absolute assignment -- from a sequence script byte as `(s8)n / 127`, from
an audio command, or `1.0` when a sequence starts -- so a port can multiply its
own setting into the field without the value compounding, as long as it treats a
value it did not write as the game's new intent.

### The microcode

| | |
|---|---|
| Blob | `aspMain`, a `textbin` in the cartridge |
| ROM | `0x8DFB0` .. `0x8EDD0` (`0xE20` bytes) |
| vram symbol | `aspMainTextStart`, `0x800D37B0`, 3616 bytes |
| **IMEM execution address** | **`0x04001080`**, not `0x04001000` |
| Data segment | ROM `0xA9310`; the command jump table's sixteen entries at `0xA9320`..`0xA933F` |

`rspboot` occupies the first `0x80` bytes of IMEM and loads the task's microcode
after itself, which is where `0x1080` comes from. Three independent checks agree,
and any one settles it:

- the microcode contains exactly three call targets (`0x1150`, `0x1184`,
  `0x11B0`); at `0x1080` all three land on the first instruction of a subroutine,
  at `0x1000` all three land mid-routine;
- the command table's first entry is the no-op handler, and at `0x1080` it
  resolves to the dispatch loop's own back-edge, which is what a no-op should do;
- the table's entries span `0x1118`..`0x1E24`; text placed at `0x1080` runs to
  `0x1EA0` and contains all of them, text placed at `0x1000` ends at `0x1E20` and
  does not contain the last.

### The command jump table

The dispatcher is `lh $2, 0x10($2)` followed by `jr $2`: sixteen halfwords at
**DMEM `0x10`**, living in the microcode's *data* segment rather than its text,
so it is invisible to any tool that discovers branch targets by walking the
instruction stream. The values, read straight out of the cartridge:

```
0x1118, 0x1470, 0x11DC, 0x1B38,
0x1214, 0x187C, 0x1254, 0x12D0,
0x12EC, 0x1328, 0x140C, 0x1294,
0x1E24, 0x138C, 0x170C, 0x144C,
```

`0x1B38` is ENVMIXER. One entry reads `0x12EF` at the moment the game uses it:
**the RSP's program counter is twelve bits and word aligned, so `jr` discards the
low two bits** and the hardware target is `0x12EC`. Anything interpreting these
targets must mask with `0x1FFC`.

### DMEM layout during an audio task

| DMEM | Contents |
|---|---|
| `0x00`-`0x0F` | constant pool, including DMA chunk-remainder bookkeeping that varies legitimately between runs |
| `0x10`-`0x2F` | the sixteen-entry command jump table |
| ... | four channel buffers of **160 samples each**, the last one flush against `0xF80` |
| `0xFC0` | the OSTask copy |
| 4 KB total | anything at or past `0x1000` wraps to `0x000`, onto the constant pool and the table |

The layout leaves **no slack at all** past `0xF80`: a store beyond it lands on
the dispatch table, and the next command then jumps to a corrupted address.

The game's own parameters, read live: target 544 samples per frame, AI buffer
528..560, 4 updates per frame, chunk 136 (128..144). A per-update chunk is capped
at 144 against the 160 the layout allows, so the ordinary synthesis path cannot
overrun; the reverb and final-interleave paths use hardcoded `0x140` and `0x280`
lengths that fit exactly.

Sample rate: the game starts at 32000 Hz and settles on **26900 Hz**. It paces
itself against how fast the audio queue drains, so a port must not let the output
device silently run at a different rate.

### Command list buffering

The game **double-buffers its audio command lists**, assuming the RSP has
finished with a list long before that buffer's turn comes round again two frames
later -- about 1 ms per task on hardware. The microcode DMAs its list in
**`0x140`-byte chunks as it goes**, so a list rewritten mid-task is executed as a
*splice* of two frames' commands rather than as one or the other. The command
format is ABI 1: eight bytes per command, opcode in the top byte.

This is a real hazard for a port rather than a theoretical one; see
[PORTING.md §6](PORTING.md#6-rsp-and-audio).

---

## Keeping this current

This file describes the *game*. When a later change to the port discovers a new
address, table, format or drawing convention -- or corrects one written here --
update this file in the same commit that makes the change. Facts about N64Recomp,
librecomp, ultramodern or RT64 belong in [PORTING.md](PORTING.md) instead.
