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

1. *Measure before tagging.* Extend the frame-rate report with RT64's count of
   unpaired transforms per frame, by game state, and collect the player's
   descriptions of what flickers and when. Expected suspects, from how the
   game draws: spray and splash particles that die and respawn (a wrong pair
   streaks between the two), buoys and scenery entering the view (a shifted
   pair for one frame), billboards built by the look-at helper, and the
   render-to-texture pass the game draws at 1:1 before the main frame.
2. *Tag by class.* Objects that respawn get `G_EX_ID_IGNORE` (drawn at the
   newer frame, never interpolated). Racers, buoys and scenery get an explicit
   ID from the matrix pool slot and the model drawn, with linear ordering, so
   pairing no longer depends on the heuristic. The camera keeps simple
   interpolation. RT64's velocity tolerance for automatic pairs is a compile-
   time constant; if a class needs a different one, it becomes another
   idempotent patch in `tools/patch_rt64.py`.
3. *What stays at the game's rate, on purpose.* The water surface's waves and
   the HUD's counters are rebuilt from vertices every game frame. The camera
   moves smoothly over the water because the water is in world space under an
   interpolated view; the waves themselves animate at 20 or 30 Hz, and that is
   how the game looks.

Verification: the unpaired count per state drops to what the class table
predicts, and a playtest of a championship, a time trial and stunt mode finds
nothing that streaks or judders that did not on the cartridge.

**Gate:** HUD Placement and Aspect Ratio each do what their names say on
every screen, and a full championship shows no interpolation artefact.

## Standing constraints

- No ROM, asset, or ROM-derived file is ever committed. The user supplies the
  dump at build time.
- Generated code is never hand-edited. If the output is wrong, fix the config
  or write a script in `tools/`.
- Every phase is entered only through the previous phase's gate.
