# Build plan

## Target revision

This project targets **Wave Race 64 (USA) v1.0** (revision 0, entry point
`0x80046800`, header CRC `0x7DE11F53 0x74872F9D`).

All existing Wave Race 64 reverse engineering targets Rev A instead:
LLONSIT's decomp declares support for "US, Rev1" only, and both public
recompilation attempts use Rev A. Targeting v1.0 therefore means no one has
walked this exact path before.

We do it anyway, on evidence rather than hope: the decomp's Rev A `entry`
segment is at vram `0x80046800`, and that is exactly the entry point in the
v1.0 cartridge header. The two revisions agree on where code is loaded, which
makes the Rev A symbol corpus a *donor* rather than a dead end -- symbols get
ported across by matching function bodies, not rediscovered.

This is a measurable assumption, and phase 01 measures it. If the segment map
turns out to diverge substantially beyond the entry point, the cost is bounded
and visible early: either absorb the extra symbol recovery, or re-pin to Rev A.
Do not let it stay an assumption past phase 01.

## The decision this project is built on

N64Recomp needs symbols and section metadata, not just a ROM. Its config
(`src/config.cpp` upstream) accepts exactly one of two input modes:

- `elf_path` — **elf input mode**
- `symbols_file_path` + `rom_file_path` — **symbols file mode**

Symbols file mode is faster to a first result and needs no MIPS toolchain. It
is also a dead end for this project: upstream rejects `func_reference_syms_file`
and `data_reference_syms_files` outside elf input mode, which means the
reference-symbol workflow and the single-file-output patch workflow — the thing
that makes phase 04 iteration take seconds instead of minutes — are unavailable
for the life of the project.

**So: we assemble our own ELF.** Not a decompilation. An *assembly-only* ELF
built from splat output, which needs symbol names, addresses and sizes but no
recovered C and no matching build. Sources for those symbols, in order of trust:

1. [LLONSIT/Wave-Race-64](https://github.com/LLONSIT/Wave-Race-64) — WIP decomp
   of this exact revision. Its splat config is the starting point.
2. A JAL-target scan over `.text` for call targets splat did not classify as
   functions. WACOMalt's equivalent pass took 756 known functions to 1,228.
3. Byte-matching libultra functions against a known SDK build, so they can be
   renamed and handed to the runtime via `reimplemented_funcs` rather than
   recompiled.

Disagreements between those three sources are bugs, not noise. Wrong function
boundaries produce C that compiles and then corrupts state at runtime.

## Phases

Each gate is the entry condition for the next phase.

### 00 — Ground rules and skeleton
Repo, submodules, `.gitignore` that refuses game data, phase-gated CMake,
toolchain, ROM identification tool.
**Gate:** the tree configures and builds; `--identify` accepts your dump.

### 01 — Split the ROM
Port and verify the splat config. Enumerate overlay segments and their load
addresses. Run the JAL scan. Rename libultra functions.
**Gate:** an assembled ELF whose `.text` is byte-identical to the ROM's code.

### 02 — First recompile
Write `recomp/wr64.toml`, run N64Recomp, compile the output. Script the fixes
for writes to `$zero` and unresolved jump tables; never hand-edit generated C.
Keep `--dump-context` output in tree as the symbol reference patches link against.
**Gate:** all `funcs_*.c` compile and link into a static library.

### 03 — Runtime harness
Register RT64. Wire VI timing. Controller callback into `OSContPad` with the
analog curve right — Wave Race is unusually sensitive to stick response. Audio
callback. EEPROM save through librecomp, to a per-user path. ROM ingest on
first run.
**Gate:** the executable reaches `recomp_entrypoint` and runs the first thread.

### 04 — Boot bring-up
The long phase, and where both public Wave Race 64 attempts are parked.
Overlay relocations first: if the ROM's overlay tables do not carry relocation
data the tooling recognizes, reverse the loader's format and drive
`recomp_load_overlays` / `overlay_apply_relocations` from a `RECOMP_PATCH`.
Expect to land on librecomp's `osEPiRawStartDma` guard repeatedly. Instrument
with a function-entry trace; a debugger on 19 MB of generated C is not a plan.
**Gate:** logo, then the attract-mode demo, rendering recognizably.

### 05 — Graphics and audio correctness
Confirm the F3DEX variant and that RT64 dispatches it. Verify the water
surface — the game's signature effect, with a documented history of breaking
under HLE renderers. Audio through ultramodern. Play every course in both
directions plus stunt mode.
**Gate:** a full championship completes with correct visuals, audio and records.

### 06 — Enhancements and release
Widescreen and arbitrary resolution through RT64, RecompFrontend for menus and
rebindable input. High frame rate without touching the game: physics and camera
are tied to a 30 Hz update, so the game keeps its rate and RT64 interpolates
each object's transform between game frames, as the other ports do. Then CI that
builds without a ROM, and a first-run flow that explains the ROM requirement.
**Gate:** a stranger with a dump and no context can build and play it.
*Met: 0.1.0, 2026-09-04.*

### 07 — 0.2: widescreen 2D, and the frames in between

Two families of visual defect are left over from 06, and one tool fixes both:
a **display-list rewriter** in the port that copies each graphics task's list
into scratch RDRAM and inserts RT64's extended GBI commands where they are
needed, before RT64 sees it. The game's code and data are untouched; the
cartridge predates the extended GBI, but RT64 honours its commands from any
list that starts with `gEXEnable`, and its Fast3D microcode leaves the opcode
free. The rewriter tracks the segment table and the matrix stack as it walks
the list, so it can read a viewport, a matrix or a vertex from RDRAM and decide
from what it finds.

**A. The 2D layer, first.** The game draws all of its 2D -- HUD, menus,
fades -- as triangles under an orthographic projection, not as texture
rectangles. RT64 keeps such a layer at 4:3 in the centre of the widened frame,
which is right for a race HUD but wrong twice over:

1. *Screens that mix 2D layout with 3D objects.* On the watercraft and rider
   select screens the 2D frames stay at 4:3 while the 3D models are drawn in
   the widened frustum, so the models land outside the frames meant for them.
   Every perspective projection drawn in a menu state gets a matrix group with
   `G_EX_ASPECT_ADJUST`, which squeezes the projection to its 4:3 proportions
   in the centre, where the 2D layout is. Race states are left widened. The
   game's state variable says which is which. This is the smallest change and
   the first deliverable: it validates the rewriter on one inserted command.
2. *HUD Placement does nothing.* RT64 anchors 2D geometry to a screen edge by
   the origin carried on its viewport. The rewriter classifies each 2D draw by
   where its vertices fall on the 320-wide screen -- left third, middle, right
   third, or spanning it -- and, around each class, pushes the viewport, sets
   a viewport alignment for that edge (with the offset that cancels RT64's own
   origin displacement, so the game's viewport data is reissued unchanged),
   widens the scissor so the anchored element is not clipped, and pops
   afterwards. Full-width backgrounds are stretched across the frame. With the
   origins in place the menu's option works as designed: *Original* pulls every
   anchor to the centre (the HUD as it is today), *16:9* anchors to a virtual
   16:9 frame, *Expand* to the display's edges. Aspect Ratio and HUD Placement
   then act independently, which was asked for in 06.
3. *The edges have to be the window's edges.* The present crop that removes
   the game's overscan borders fits the 303x199 drawn region to the window's
   height, so the widened frame overhangs the window by a tenth of its width on
   each side, and an element anchored to RT64's left edge would sit off screen.
   The crop changes to fit the widened frame's width to the window's, with the
   borders still cropped; the same picture in fullscreen 16:9, and edges that
   mean what they say.

Verification: captures of title, main menu, options, rider and watercraft
select, course overview, a race HUD and the pause menu, in each HUD Placement
mode, at 16:9 and in a 4:3 window. Nothing may be cut off or drawn twice.

**B. The frames in between.** RT64 pairs each object's matrix with the previous
frame's by draw-call signature and nearest position, and pairs about 98% of
transforms; the rest, and the wrong pairs, are the flickers seen during play.
The rewriter is the fix here too: it can tag any matrix with an explicit group.

1. *Measure before tagging.* Done. `tools/patch_rt64.py` has RT64 count, per
   frame, how many world transforms there were, how many found no pair, and how
   many of those had a matrix that appears nowhere in the previous frame;
   `patches/framerate.cpp` reports the rates on the same two-second window as
   the frame rate, under `WR64_PAIRING=1`.

   What it says, measured in a race: 100 to 200 world transforms a frame, of
   which 2 to 3 find no pair and 1 to 2 of those are moving or new. The rest
   are the course's static scenery -- the same matrix every frame, often
   submitted twice, which is exactly what ties a matcher that goes by position
   -- and one transform with no address that never pairs in any frame. None of
   those cost anything: an unpaired transform is drawn at its current matrix
   (`TransformProcessor::process`), so an object that is not moving looks no
   different for it. The plain count of unpaired transforms is therefore the
   wrong number to chase, and the second count is in the report for that
   reason. Whole-frame failures do happen -- 52 of 52, 142 of 142 -- but only
   at scene cuts, where nothing should be interpolated anyway.
2. *Tag by class.* Only where the measurement asks for it. So far that is the
   sky and the water, which are drawn under the game's identity matrix: several
   transforms with the same matrix are what ties the matcher, and both now
   carry an explicit ID with linear ordering, matched by identity before the
   heuristic runs. Objects that respawn would get `G_EX_ID_IGNORE` (drawn at
   the newer frame, never interpolated) if the report names them; the spray and
   splash particles rebuilt through segment 5 every frame are the candidates,
   and they have not been tagged, because nothing yet says they need it.
3. *The sky, and the water.* Done, and they are the same defect. The sky is
   drawn the same way the world is -- under an
   identity model matrix, with the camera on the projection stack -- but it
   is not part of the world: the game rebuilds its three bands' vertices into
   a scratch buffer behind segment 6 every frame, following the camera and
   scrolling the clouds. The matrix RT64 interpolates is therefore identical
   from frame to frame, it pairs perfectly, finds no motion, and holds the sky
   still between the game's frames, which is the shimmer along the horizon at
   60 Hz. The rewriter gives that section a matrix group asking for the
   vertices and their texture coordinates to be interpolated as well; RT64
   then carries a per-vertex velocity, and the sky moves with everything else.

   The water surface is the same story and was fixed the same way. It is a
   lattice of rows marching away from the camera, fifty vertex blocks reached
   through segment 3 under that same identity matrix, and the game recomputes
   it every frame: across four consecutive race frames, forty-one of the fifty
   carry different vertex data each time while the course and the models
   (segments 1, 8 and 13) are byte-for-byte identical. Interpolating a rebuilt
   mesh is only safe when index i means the same point in both frames, which is
   why this was left alone until it was checked -- and it holds: every block's
   vertex count is the same every frame, and every block's first vertex is
   bit-for-bit identical, so the lattice is fixed and only the heights on it
   move.
4. *What stays at the game's rate, on purpose.* The HUD's counters, which are
   2D and are drawn without interpolation deliberately, and anything drawn at a
   scene cut, where nothing should be carried across.

Verification: `WR64_PAIRING=1` reports what fails to pair, and a playtest of a
championship, a time trial and stunt mode finds nothing that streaks or judders
that did not on the cartridge. Both sections have a switch --
`WR64_NO_SKY_INTERP=1` and `WR64_NO_WATER_INTERP=1` -- because whether
interpolating a rebuilt mesh is an improvement or makes it swim is a question
only a side-by-side can answer.

**Gate:** HUD Placement and Aspect Ratio each do what their names say on
every screen, and a full championship shows no interpolation artefact.

## Standing constraints

- No ROM, asset, or ROM-derived file is ever committed. The user supplies the
  dump at build time.
- Generated code is never hand-edited. If the output is wrong, fix the config
  or write a script in `tools/`.
- Every phase is entered only through the previous phase's gate.
- The technical reference is kept current. Any change that discovers a fact
  about the game, or fixes something in the toolchain, runtime or renderer,
  updates `docs/GAME-INTERNALS.md` or `docs/PORTING.md` in the same commit --
  game facts in the first, port facts in the second. The phase findings stay as
  they are: they are the working record, including the wrong turns, and the
  reference is what someone else can use without reading it.
