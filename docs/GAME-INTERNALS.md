# Wave Race 64 internals

Everything this port had to learn about the game itself, in a form a
decompilation or another port can use directly. It is a reference, not a
narrative; how each fact was established is in the phase findings
(`docs/PHASE01-FINDINGS.md` through `docs/PHASE05-FINDINGS.md`).

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
port widening the frame has to get all four tiles out to its edges. **This port
does not yet.** The wipe stays boxed in the middle 4:3 with the previous screen's
background standing either side of it, and anchoring the tiles to both edges of
the frame placed them across it in the renderer's own arithmetic without changing
what reached the screen -- so something after placement is still deciding the
width. Unsolved.

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

**It is a rigid carry, not a recentring.** Every block moves by the same step in
every frame measured, so index *i* still names the same lattice slot, and the
correspondence a renderer pairs on is intact.

**The wave pattern is carried with it.** A 60-unit carry changes about as many
heights as standing still does (15.1 against 12.2 of 50). Were the surface
sampled from a world-fixed wave field, moving it 64 units would have changed
nearly all of them. The heights travel with the lattice; what changes frame to
frame is the animation on top.

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
