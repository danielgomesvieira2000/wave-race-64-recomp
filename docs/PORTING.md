# Porting notes

What this project hit while building a static recompilation on N64Recomp,
librecomp/ultramodern and RT64, written for whoever does the next one. Almost
nothing here is specific to Wave Race 64; where a fact is, it is marked.

The companion document, [GAME-INTERNALS.md](GAME-INTERNALS.md), holds what is
specific to the game -- addresses, tables, formats, drawing conventions.

Each section states the symptom first, because a symptom is what you will have
when you come looking.

Pinned upstream revisions, for reference:

| Submodule | Commit |
|---|---|
| N64ModernRuntime | `cdf5abbd5026fef5c364c676e4667c45e42b6863` |
| RT64 | `5473732a822a4423b5696e7cb18fecc425a59875` |
| RecompFrontend | `b1a1477c6556aeb7ed45defbfb5924f721efebc1` |

Four of the fixes below are patches to those submodules, applied by scripts in
`tools/`. They are **scripted and idempotent on purpose**: a submodule update
reverts a hand edit silently, and the resulting failure -- a build that stops, a
port that goes mute -- points nowhere near its cause.

---

## 1. Choosing the input mode

N64Recomp accepts exactly one of two input modes: `elf_path`, or
`symbols_file_path` + `rom_file_path`. Symbols-file mode is faster to a first
result and needs no MIPS toolchain, and it is a dead end for anything beyond
that: upstream rejects `func_reference_syms_file` and `data_reference_syms_files`
outside ELF input mode, which removes the reference-symbol workflow and the
single-file-output patch workflow for the life of the project.

The patch workflow is what makes bring-up iteration take seconds instead of
minutes, several hundred times over. **Assemble an ELF.** Not a decompilation --
an assembly-only ELF from splat output, which needs symbol names, addresses and
sizes but no recovered C and no matching build.

---

## 2. Assembling a byte-exact ELF from splat output

**Symptom:** the ELF links, looks right, and each code section differs from the
ROM in a fraction of its bytes -- 0.5% to 9% here, so 91-99% correct, which means
the segment map and section placement are right and something narrower is wrong.
Comparing a section against its ROM offset shows differences scattered
everywhere, including in R-type instructions that have no immediate field to
relocate at all.

There is normally only one bug behind that, and it is not a relocation bug.

### splat drops trailing bytes

**splat emits each data subsegment only as far as its last symbol.** Trailing
bytes covered by no symbol are simply not written. The linker then concatenates
the next object immediately, so every shortfall shifts everything after it, and
the shifts accumulate -- in this game to `0x3080` by the end of `main_segment`.

Once data drifts, a byte-for-byte comparison at a fixed ROM offset is misaligned
from that point on and every later word looks wrong, R-type instructions
included. That is where the false "relocation values are wrong" lead comes from.

The fix is in three parts:

| Fix | Tool | Recovered here |
|---|---|---|
| Pad each data subsegment to its true length -- from its own ROM start to the next subsegment's -- with **`.incbin` of the exact ROM range** | `tools/pad_data_objects.py` | 27,056 bytes |
| Pad each segment's tail, for bytes belonging to no subsegment at all (every overlay ended this way). Only visible by comparing a linked ELF against declared segment sizes, so the build runs twice | `tools/pad_segment_tails.py` | 1,552 bytes |
| Pin each bss object to the address splat recorded, in the linker script -- bss objects do not tile a contiguous range, so padding them would invent hundreds of kilobytes | `tools/fix_asmonly_ld.py` | -- |

`.incbin`, not `.space`: `.space` invents zeros and produces an ELF that looks
padded while being wrong.

### Two fixes that look obviously right and do nothing

Both were measured, twice, and neither changed the output by one byte:

- **`migrate_rodata_to_functions: False`** -- inert. It only affects `c`
  subsegments, and an asm-only config has none.
- **`subalign: 16`** -- inert. splat already emits `SUBALIGN(16)` on every
  section; the padding is missing *inside* sections, between objects.

And one that looks right and actively regresses the build: **preferring
`<name>.rodata.s` over the text object when padding.** With rodata migration on,
the generated linker script takes `.rodata` from the *text* object and never
references the standalone file, while taking `.data` from the standalone file.
The mapping has to follow the linker script, not the filenames. Getting this
"right" took a working build back to 51,207 differing bytes.

### Verify with a measurement, not an impression

Two numbers, both cheap, and both zero when the ELF is faithful:

```
segments checked : 32     wrong size : 0     wrong bytes : 0
symbols placed   : 2551/2551 exact
```

The second is the one that localises a fault: track every symbol whose *name
encodes its own address* (`D_80151BE0`, `func_801ED338`) and compare where the
linker placed it. Drift that grows monotonically across the address space is the
signature of the dropped-bytes bug, and the first symbol that slips names the
subsegment to look at. This project's first slip was exactly `0x10` bytes of
cartridge content between `Seed` and the next subsegment that no symbol covered.

### Start from the smallest failing case

`.entry` is `0x50` bytes and only two words differed. Both were the `addiu` half
of a `lui`/`addiu` pair, both low by exactly `0x3080`, and one of them was the
boot code **setting the stack pointer** -- the port would have started with a
stack 12,416 bytes below where the game expects it and faulted much later,
nowhere near the cause. Two words in a 0x50-byte section explained 24,287
differing words elsewhere.

---

## 3. Running N64Recomp

**Run the recompiler under Linux.** The Windows build dies with `0xC0000409` -- a
`__fastfail`, here a stack overflow -- partway through writing its output, and
this is a nasty failure because it still writes most of its files and looks like
it worked: `funcs.h` simply ends mid-token (`void func_801DEC00(uint8`). Linux
gives an 8 MB default stack against Windows' 1 MB. Guard against it anyway by
checking that `funcs.h` ends in `#endif`.

### A linker-script assignment can override an object symbol

**Symptom:** `No function found for jal target: 0x801ED338`, for a function your
assembly clearly defines.

splat's `undefined_funcs_auto.ld` contained `func_801ED338 = 0x801ED338;` while
an object defined the same symbol. A linker-script assignment wins, producing an
`ABS` symbol with no section, and N64Recomp cannot resolve a call to a symbol
whose section is unknown. Drop any entry there that your own objects define
(`tools/fix_asmonly_ld.py`); the genuine entries are overlay entry points.

### Relocation types outside the enum

**Symptom:** `recomp_overlays.inl` is emitted with entries reading `.type =  },`
which do not compile.

N64Recomp casts an ELF relocation type straight to its own `RelocType`, which
covers 0-7, with no range check. `R_MIPS_PC16` is 10, so the name lookup indexes
past an eight-entry table. These relocations exist because splat declares
functions with `glabel`, making them global, and GNU as emits a relocation for a
branch to a global symbol even when it resolves inside the same section.

`tools/fix_overlay_relocs.py` discards them **after verifying every one is
section-local** -- which makes them genuine no-ops, since relocating a section
moves a PC-relative branch and its target together. It refuses to discard
anything if one turns out to cross sections.

### Three symbol lists produce `_recomp` calls, not one

**Symptom:** the generated code calls `<name>_recomp` functions that `funcs.h`
does not declare; under C99 those are implicit declarations and the build fails.

N64Recomp renames the libultra functions it delegates to the runtime, and the
names come from **three** lists: its built-in `reimplemented_funcs`, plus
`ignored_funcs` and `renamed_funcs`. Reading only the first misses
`__osPfsSelectBank` and `__osContRamRead`, and fails identically.
`tools/gen_reimplemented_decls.py` reads all three (440 declarations) and is
wired in through the `recomp_include` config option. Declaring a name that is
never called costs nothing; missing one is a build error.

### Do not list functions the tool already handles

**Symptom:** `Function __osExceptionPreamble is set as ignored in the config file
but does not exist!` -- for a function that plainly exists.

The rename removes the original name from the lookup the config check uses, so
listing a built-in reimplemented function is an error rather than a precaution.
What *does* need an entry is a symbol splat invented that the tool cannot know
about: here `func_800CB0A8`, the tail of `__osException` that splat split off,
whose hand-written assembly branches back inside `__osException`.

### `ignored`, not `stubs`, for a function you patch

**Symptom:** lld-link reports a duplicate symbol for a function you provided a
`RECOMP_PATCH` for.

`RECOMP_FUNC` is `extern inline __attribute__((weak, noinline))` under Clang, and
clang-cl takes that branch, so a strong definition should win. On PE/COFF it does
not when the function was *stubbed*: the stub is emitted into the recompiled
library, the archive member holding it gets pulled in for the other functions
sharing its object file, and both definitions reach the link. `ignored` emits
nothing at all, leaving the patch as the only definition -- and the callers still
need a declaration, so whatever generates your declarations must also read the
`ignored` list.

### Scanning for unresolved call targets

If you scan for `jal` targets that have no function symbol, scan **inside
function bodies only**. Scanning whole sections decodes any embedded data as
instructions, and an N64 game's text range routinely contains RSP microcode
blobs (`aspMain`, `rspboot`, `f3d` here) whose words decode into invented call
sites. That mistake reported 775 unresolved targets where there were 24.

---

## 4. Overlay dispatch

**Symptom:** `No function found for jal target: 0x802C744C` at recompile time,
for calls straight into the overlay window.

`resolve_jal` never treats a function in a relocatable section as a candidate
from another section, and it is right not to: which overlay is resident is a
runtime fact. Here 19 sections share one address, so a target address alone
cannot name a callee.

The escape hatch exists but is not reachable.
`Context::use_lookup_for_all_function_calls` makes every call resolve by address
against whatever is loaded, which is exactly the semantics an overlay needs. The
field and the `resolve_jal` code path both exist; there is simply **no config
key**, here or upstream, so the CLI can never set it.
`tools/patch_n64recomp.py` adds the key -- three insertions following the pattern
`trace_mode` already establishes.

The alternative is hand-transcribing the dispatchers. Here that meant a
302-instruction function with a 104-entry jump table, and two companions: far
more code, and far more to get subtly wrong.

**Turning the option on breaks two assumptions**, both of the same shape --
things that never needed an address suddenly do.

### Runtime-provided libultra functions have no address

**Symptom:** `Failed to find function at 0x800C6300`, which is `osDpSetStatus` --
a function librecomp does implement.

N64Recomp renames these to `<name>_recomp` and never puts them in a section
table, because with direct calls nothing looked them up by address.
`tools/gen_runtime_func_table.py` pairs each with the address it had on the
cartridge; 73 are registered here.

Build that list from actual `<name>_recomp` **definitions** found in librecomp
and your own sources, not from N64Recomp's 440 names. Declaring an unused
function is free; taking its address forces the linker to find a definition, and
most of those names have none.

### Resident sections are never "loaded"

`init_overlays()` does not populate the function map at all -- it only records
where sections live. Functions are added by `load_overlay()` when the game DMAs a
section in, which is sufficient while only overlays are looked up by address.
With every call a lookup, `main_segment` and `codeseg` are never registered
because they never arrive by PI DMA. All 1,088 of their functions must be
registered up front.

Two ordering traps, both of which produce silence rather than an error:

- **Register from the game's `on_init` hook, not at startup.** `init_overlays()`
  begins with `func_map.clear()`, and librecomp calls it long before
  `on_init_callback`.
- **Identify an overlay by its address being shared, not by consulting
  `overlay_sections_by_index`.** That table's values are not section indices --
  they ran 3..21 here while `main_segment` is index 8 and `codeseg` is 11 -- so
  using it as an index set silently skips most of the game. It registered 150
  functions instead of 1,088 and looked entirely plausible.

Overlay sections must be *excluded* from the up-front pass: several share one
address, so registering them installs whichever came last and defeats the
dynamic dispatch.

### Announce every load to the runtime

librecomp announces only the boot region:

```c
load_overlays(0x1000, (int32_t)entrypoint, 1024 * 1024);
```

Everything after that is the game's business. Hook the lowest point both of the
game's loading paths pass through -- `osPiStartDma` here -- and call
`load_overlays` from it. **Wrap librecomp's implementation rather than replacing
it:** read the arguments *before* the call, since the callee owns the context and
may leave the registers holding anything.

Three details that each cost a debugging session:

- **Register the hook last.** `osPiStartDma` is a reimplemented libultra
  function, so it appears in both the runtime-provided table and the resident
  pass. Installed with the first, it is overwritten by the second moments later,
  and nothing is ever announced.
- **Two address conventions.** The ROM argument is normalised the way librecomp
  does it, `(addr | rom_base) & 0x1FFFFFFF`, which accepts a bare file offset or
  a K1 cartridge address without knowing which the caller used; `load_overlays`
  then wants the file offset, since that is the space the section table uses. And
  a game's DMA helper may take a **physical** RAM address (this one does), while
  `do_rom_read` writes through `MEM_B` and wants KSEG0 -- handing the physical
  address over faults two gigabytes past RDRAM.
- **Skip `OS_WRITE`.** A transfer back to the cartridge cannot bring code in.

### Make the load synchronous

**Symptom:** an overlay lookup fails for code that is legitimately not there yet.

A game's DMA helper is usually asynchronous in shape: it starts a PI transfer and
waits on a message queue. Recompiled faithfully, the data does arrive -- but the
renderer thread reached its first overlay call before the loader thread had
transferred anything. Replacing the helper with a synchronous `do_rom_read`
removes the race entirely, and is stronger than the game asks for. If you do
this, drop the queue waits with it: nothing sends to those queues any more, so
the closing wait would block forever.

---

## 5. The runtime harness

### Everything RT64 declares is directory-scoped

**Symptom:** four unrelated-looking failures -- `lld-link: could not open
'SDL2.lib'`, hlsl++ failing to parse with hundreds of errors, RT64 headers not
found, and `0xC0000135` at startup with no message at all.

One cause. RT64 sets its compile definitions, include paths, link paths and DLL
copies with `add_compile_definitions`, `include_directories`, `link_directories`
and `configure_file`, all of which apply to targets declared inside RT64's own
`CMakeLists.txt` and none of which reach a target declared outside it. The
library *names* do propagate through `target_link_libraries`, which is what makes
it confusing: the link asks for `SDL2.lib` while having no idea where it lives.
Repeat each on your own target.

The DLL case is the nastiest: Windows reports only a status code, naming neither
the missing DLL (`dxcompiler.dll`, `dxil.dll`) nor the fact that one is missing.

### Use clang-cl, not clang++

RT64 chooses its warning flags from `CMAKE_CXX_SIMULATE_ID`, so a Clang targeting
the MSVC ABI is handed `/W4` on the assumption that such a Clang is `clang-cl`.
`clang++` reaches the same ABI through the GNU driver and rejects `/W4`, failing
every RT64 translation unit. Changing compiler also invalidates the CMake cache,
and `-D` options do not survive that -- reconfiguring in place silently comes back
with your options switched off.

### Build RelWithDebInfo, not Debug

**Symptom:** rare audio clicks; roughly one audio task in a thousand corrupted.

`CMAKE_BUILD_TYPE=Debug` in a build directory's cache overrides a project's
default, and the recompiled RSP vector unit is very slow unoptimized: an audio
task took 5 to 24 ms against a 16.7 ms frame, instead of about 1 ms. That is
enough to lose the game's double-buffering assumption; see §6. State the build
type explicitly in your build instructions.

### `start_game()` comes before `start()`

**Symptom:** the process lives happily with a window open, prints nothing, and
never reaches the entry point -- with no error anywhere.

`recomp::start()` spawns the game thread, which spins in `wait_for_game_started`,
and then enters its own main loop, which does not return until the user quits.
Calling `start_game()` after it therefore never runs. `start()` also creates the
window when none is supplied, so creating one in advance just initialises SDL
twice.

### RT64 must be told the microcode before a display list

**Symptom:** `Assertion failed: hleGBI != nullptr, rt64_interpreter.cpp, line 157`
on the first display list.

`processDisplayLists` does not select the graphics binary interface itself. An
emulator leaves the task in DMEM for RT64 to read; a recompilation has no real
DMEM, so nothing identifies the microcode. Call
`interpreter->loadUCodeGBI(task->t.ucode, task->t.ucode_data, true)` first,
passing the addresses straight from the OSTask.

### A virtual address is not an RDRAM offset

RT64 indexes straight off the RDRAM base. An OSTask carries KSEG0 addresses, and
handing one over unmasked indexes two gigabytes past an 8 MB allocation. Mask the
segment bits off (KSEG0 and KSEG1 are both direct-mapped; a game that does not
use the TLB needs nothing more).

### Six libultra functions belong to nobody

`ignored_funcs` marks routines N64Recomp will not translate because the runtime
provides them -- but librecomp does not implement `osPfsIsPlug`, `osPfsInit`,
`__osPfsSelectBank`, `__osContRamRead`, `__osContRamWrite`, `__osGetCause` or
`send_packet`. They surface as undefined symbols at link time with nothing to say
whose job they were.

Answer the five Controller Pak routines with `PFS_ERR_NOPACK`, which is what the
game would see on hardware with an empty controller slot -- librecomp answers its
own share of that API the same way, and a game's Pak-detection path then takes
its "no Pak" branch, which is a real code path rather than a placeholder.
`send_packet` is libultra's kernel debug server, talking to development hardware
over a link no player has.

### One owner for the SDL event queue

**Symptom, with a frontend library in the picture:** "vector subscript out of
range", process gone immediately and uncatchably, on the first gamepad button
press. Keyboard input never triggers it.

`SDL_PollEvent` removes what it returns, so two polling loops cannot coexist: the
second never sees anything. A hand-rolled loop that merely forwarded events to
the UI skipped bookkeeping later event handling depends on -- concretely, it never
registered a connected controller with the input-profile system, so
`get_input_profile_for_player` kept returning -1, and the first button press
indexed a profile vector with that. Let the frontend's own loop be the only
drain, and find controllers by rescanning (`SDL_GameControllerOpen` on an
already-open device returns the existing handle, so rescanning is cheap and
correct).

### The stick the runtime expects is normalized

**Symptom:** steering is on or off. The craft turns as hard as it can or not at
all, a gentle lean does what a full one does, and the pad is indistinguishable
from the keyboard. Nothing in the port looks wrong: the axis is read, a deadzone
is applied, and a plausible value is handed over.

`ultramodern::convert_to_n64_range` (`ultramodern/src/input.cpp`) is what turns
the `get_input` callback's `x` and `y` into the `int8_t` pair the game reads. It
takes the **magnitude** of that pair, clamps it to `1.0`, and then scales by the
stick's own radius (about 82) through the octagonal gate:

```c
float magnitude = sqrtf(x * x + y * y);
if (magnitude > 1.0f) magnitude = 1.0f;
...
output_magnitude = magnitude * n64_radius / square_radius;
```

So the callback must return a **normalized** pair, not the N64's own ±80. Handing
it ±80 puts every deflection past the deadzone over the clamp: the angle survives
the conversion and the magnitude does not, and an analogue stick arrives at the
game as eight directions at full lock. The failure is invisible in a log -- the
values going in are the right shape and the values coming out are legal -- and it
is worst in a game like this one, where steering is analogue throughout.

Keep the ±80 range inside the port if the scripted-input files and the game's own
constants are written in it, and divide by it once, at the callback boundary.

### A setting the frontend defines is not a setting the frontend applies

**Symptom: a slider in the settings menu does nothing.** recompui's Sound tab
defines **Main Volume**, stores it, restores it between sessions and exposes
`config::sound::get_main_volume()` -- and nothing upstream ever reads it. The
same is true of Rumble Strength, which recompinput does consume, but only for
the motor: every other tab's values are the port's to act on. Grep the library
for the accessor before assuming a control is wired: if the only callers are the
tab that created it, the port is the missing half.

Two details make it feel right rather than merely work:

- **Hook the option's change callback rather than polling.** It fires with
  `Load` when the saved value is read at startup, `Temporary` while the slider is
  being dragged -- which is what makes the volume audibly follow the handle --
  and `Permanent` on Apply. Polling gives you the first and the last only.
- **Apply it where the samples already are.** This port copies every buffer once
  to undo a channel swap, so the volume is one multiply inside a loop that was
  running anyway. The value crosses from the UI thread to the audio thread, so
  it is an atomic; the config API is not something to call per buffer.

And a warning for anyone doing this late: **a control that has never worked has
been set by people who could not hear the result.** This project's own saved
setting was zero. Making the slider work made the port correctly silent, which
is indistinguishable from breaking the audio unless the log says which it is.

### Separating one sound from another after the mix is impossible

**So do it before the mix.** A main volume can scale the finished buffer; a music
or voice volume cannot, because the microcode has already mixed everything into
one stereo stream by the time a port sees it. Both have to reach into the game's
audio engine, which for this era usually means the sequence-player engine with
its `gSequencePlayers` array: music and effects are separate *players*, and
within a player, separate *channels*.

Two fields are the hooks, and both are ones the sequences themselves write:

| Level | Field | Recompute asked for by |
|---|---|---|
| player | `fadeVolumeScale` | the player's `recalculateVolume` bit |
| channel | `volumeScale` | the channel's `changes.volume` bit |

Because the game writes them too, **compose rather than replace**: shadow the
value, treat anything you did not write as the game's new intent, and write that
base times your setting. Every write the game makes to them is an absolute
assignment, so nothing compounds. Replacing instead would break the game's own
ducking -- this one drops the title music to `0.55` under the menu.

**Finding which channel carries one particular sound is harder than it looks,
and a correlation is not an identification.** This port tried and failed to
separate the announcer's voice, and the failure is the useful part.

The method looked sound: log every channel's activity per frame against the
game's own tick, log gameplay events with the same tick from the feedback code,
and line the two up. One channel of the effects player began three frames after
the countdown started and ran for the 144 frames of "three, two, one, go", spoke
again on the course screens and after a retirement, and was idle in between,
while every other channel either ran continuously (the engine, the water) or
fired in short bursts at splashes and collisions. No other channel had that
shape.

It was still the wrong channel. Scaling it to zero was verified in the trace --
`volumeScale` and `appliedVolume` both at zero for hundreds of consecutive
frames -- and the announcer kept talking. What the correlation had actually found
was a countdown *sound* that happens to start when the announcer does.

Two lessons, in order of how much time they cost:

- **A trace can prove a write landed and still say nothing about what it is
  heard as.** Separating one sound from another needs an ear in the loop at some
  point. `WR64_AUDIO_MUTE` exists for that: it silences a whole player or a
  single channel from the environment, so a candidate can be ruled out in one
  race rather than one build.
- **Do not assume the shape of the engine from one screen.** At the title only
  two players are ever enabled, which is what the first traces showed, and the
  rule "sequence ids from 3 up are music" was written from that. In a race a
  *third* player is running, and the sequence ids there are nothing like the
  menu's -- so a rule keyed on the id alone mislabels what it scales.

### Players, pads and profiles

**Symptom: the controls tab's remapping does nothing.** A port that reads SDL
buttons in its own `get_input` callback -- the obvious way to write it, and what
this one did -- never consults the bindings the player edited, because those live
in recompinput's profiles. `profiles::get_n64_input(player_index, buttons, x, y)`
is the function that applies them: it merges the player's controller and keyboard
profiles, applies the deadzone from the settings, and returns the stick already
normalized to ±1, which is the range the runtime wants. Call that instead, once
per player index, and remapping, per-device profiles and the second player all
start working at once.

**Symptom: the keyboard does nothing while a gamepad works perfectly.** The pad
is bound, the profiles are assigned, the settings look right, and not one key
registers. `recompinput::poll_inputs()` is what copies SDL's keyboard state into
the library each frame, and -- like `update_rumble` -- it is exposed for the port
to call rather than called by the library itself. Without it `InputState.keys`
stays null and every keyboard binding reads as unpressed, while the controller
path keeps working because it asks the player's own SDL handle directly. Nothing
in the profiles or the settings hints at it. Call `poll_inputs()` once per frame,
next to `handle_events()`.

**And when input moves onto the frontend's profiles, its default bindings come
with it.** A port that read SDL scancodes itself had its own keyboard layout, and
the moment `get_n64_input` takes over, RecompFrontend's defaults replace it
silently -- a keyboard that works, on keys nobody documented. Declare the port's
layout with `set_default_mapping_for_keyboard` **before**
`recompui::config::finalize()`, which is what loads the controls file and creates
the profiles. Defaults only reach a profile the first time it is created, so
anyone with a saved profile keeps what they have until they reset it.

**Symptom: rumble does nothing until someone opens the controls tab and assigns a
pad.** `update_rumble` iterates *assigned players*, and nothing is assigned until
an assignment has been committed, which by default happens only through the
modal. Meanwhile the pad plays the game fine, because the port was reading it
directly, so the two halves disagree about whether a controller exists.

The frontend's own model is a modal because it is written for games where which
pad is which matters. Two smaller pieces make it automatic:

- `players::set_player_count_range(min, max)` at startup, so the controls tab
  offers the number of players the *game* has rather than the default four.
- Assigning pads in connection order rather than by button press. There is no API
  for that upstream, so this project adds one in
  `tools/patch_recompinput.py` (`players::auto_assign_controllers`), which does
  what committing a manual assignment does -- fill the player list and give each
  player the profile belonging to its pad -- from a list the port supplies. The
  port calls it whenever the set of connected pads changes, and it declines while
  a manual assignment is open so the modal still wins.

With no pad attached, assign the keyboard to player one, or a keyboard-only
machine has no players and `get_connected_device_info` reports an empty port to
the game.

### Rumble for a game that has none

Wave Race 64 (USA, Rev A) predates the Rumble Pak, and `Motor` appears nowhere in
its disassembly outside the SDK header that declares `osMotorStart`. There is
nothing to pass through, so the feedback in this port is worked out from the
game's own state (`src/haptics.cpp`, and [GAME-INTERNALS.md §7](GAME-INTERNALS.md#7-reading-a-race-from-rdram)
for what it reads). Four things are worth knowing before doing the same:

- **RecompFrontend already has the control and the feel.** `has_rumble_strength`
  on the general tab gives a 0-100 slider, and `recompinput::update_rumble` ramps
  the motor towards full by 0.17 per update while an effect is wanted, decays it
  by ×0.92 after, smoothsteps the result and scales it by the slider. Zero on the
  slider is off, so a separate on/off toggle is a second control for a state the
  first already expresses. **The whole path is skipped unless the option is
  declared**, so that one flag is also what turns the feature on.
- **Nothing calls `update_rumble` for you.** It is exposed for the port to call
  once a frame; without that, `set_rumble` only ever sets a flag.
- **Its interface is a bool, not an amplitude**, which is what a Rumble Pak was:
  a fixed-speed motor a game switched on and off. So an effect's strength is the
  *length* of the pulse asked for -- 30 ms is a tick, 200 ms a thump -- and that
  is a feature rather than a limitation, since it is how the games being ported
  shaped their own feedback. Pulses shorter than about 100 ms are where that
  scaling lives: at 0.17 a frame the ramp reaches full in six, so anything longer
  is already at the ceiling and only lasts longer.
- **Symptom: everything feels light, even with the slider high.**
  `recompinput::update_rumble` calls `SDL_JoystickRumble(joystick, 0, strength,
  duration)` -- zero for the **low-frequency motor**, which on most pads is the
  big, heavy one, and the strength on the high-frequency one, which is the small
  buzzing one. Every effect a port sends through it is therefore a buzz by
  construction, and no amount of pulse length or slider will make it a knock.
  Fixing that means patching recompinput to drive both motors, which is a change
  to a submodule and so belongs in a script under `tools/` like the others.
- **Read the game on the game's thread, drive the motor on the main one.** The
  frame's `osViSwapBuffer` is the point where the game's update has finished and
  its memory is consistent, and a port's hook there is already on its thread; SDL
  belongs to the main loop. In between, pass timestamps, not pointers into RDRAM.
  Note that the input-poll callback is not the main thread -- the game calls it
  too, through `osContStartReadData`.

### Mods are already running before a port does anything

`recomp::start` calls `recomp::mods::initialize_mods()` and `scan_mods()` itself,
creates `mods/` and `mod_config/` in the settings folder and reads `mods.json`.
A port supplies one thing for that to work -- `game.mod_game_id` on the
`GameEntry` it registers -- and this port had set it from the beginning, so mods
were being scanned for a year of releases with no way to see the result. A mod
could be installed and never appear, never be enabled, and never report why it
failed to open.

What was missing was two calls: `recompui::config::create_mods_tab()` beside the
other tabs, and `add_mods_option()` on the launcher's game-options menu. The tab
itself, the install button, the mods folder button, per-mod options, enable and
reorder are all RecompFrontend's.

What ships in the runtime:

| | |
|---|---|
| Container | `.nrm`, a zip, manifest required (`manifest.json`) |
| Content: code | `mod_binary.bin` + `mod_syms.bin`, recompiled live at load |
| Content: ROM patch | `patch.bps` |

This port registers one: **`rt64.json` means a texture pack**. RT64 has a
complete replacement system and reads a pack out of a zip without unpacking it,
so a `.nrm` *is* a pack -- but RT64 has no way to be *told* which packs to load
by the program embedding it; the only path in is a file dialog in its developer
UI. `tools/patch_rt64_texturepacks.py` adds `RT64_SetTexturePacks`, which records
a list and lets `State::updateScreen` apply it on the next frame -- the next
frame rather than immediately, because a mod can be switched on from another
thread while the game runs and loading a pack rebuilds the texture cache.

Two things that cost a run each to find: the manifest inside a mod is
**`mod.json`**, not `manifest.json`, and `enabled_by_default` applies **only the
first time the game sees a mod id**. After that the state lives in `mods.json`,
so a mod already installed ignores the field -- which looks exactly like the
enable callback not firing.

**A port can register content types of its own** with
`recomp::mods::register_mod_content_type({ content_filename, allow_runtime_toggle,
on_enabled, on_disabled, on_reordered })`. A mod is then detected as carrying
that content simply by containing a file of that name, and the port is called
when it is enabled or disabled -- with `allow_runtime_toggle` it can be flipped
without restarting. That is the cheap way to make a mod format for data the port
already understands, without any of the code-mod toolchain.

### Where settings go

librecomp writes settings, profiles and mod state wherever it is told, and until
it is told, that is the current working directory: start the game from a
different directory and every setting comes back as its default. Register the
config path before anything loads a setting, and make sure the frontend sees the
same one -- these are two separate registrations and they drift apart silently.

---

## 6. RSP and audio

The RSP recompiler is a separate tool (`RSPRecomp`) driven by its own TOML, and
it emits **C++**, not C, because the generated code uses librecomp's RSP vector
unit -- a C++ header of SSE intrinsics.

### `text_address` is the IMEM address, and it is probably not `0x1000`

**Symptom:** the recompiler produces a complete, plausible-looking file that
compiles and links, and the game freezes with no diagnostic at all.

`rspboot` occupies the first `0x80` bytes of IMEM and loads the task's microcode
after itself, so an audio microcode usually executes at **`0x04001080`**. Set to
`0x04001000`, every jump lands `0x80` bytes early. What that does at run time is
subtler than a crash: the microcode's first call -- the one that DMAs the command
list into DMEM -- lands in the middle of an unrelated routine, the dispatcher
reads an empty command buffer, and it spins forever. The RSP task thread never
returns, the scheduler waits for an SP-complete event that will never come, and
every other thread carries on: the audio thread keeps building tasks, the window
keeps swapping the same finished frame. **The port looks like it is running.**

Verify the address before trusting it, with checks that do not depend on it:
every call target should land on the first instruction of a subroutine; the
command table's no-op entry should resolve to the dispatch loop's back-edge; and
the text, placed at that address, should contain every jump table entry.

### Jump tables in the data segment are invisible

**Symptom:** the first audio task returns `UnhandledJumpTarget`.

RSPRecomp discovers branch targets by walking the instruction stream, so a target
that exists only as data is invisible to it. Read the table out of the cartridge
and list the values in `extra_indirect_branch_targets`.

### The RSP ignores the low two bits of an indirect jump

**Symptom:** `UnhandledJumpTarget`, then librecomp asserts, the RSP task thread
dies, and no task of *either* kind is ever started again.

The RSP's PC is twelve bits and instructions are word aligned, so `jr` discards
the low two bits of its register. RSPRecomp's generated dispatch switches on the
raw value, `switch ((jump_target | 0x1000) & 0x1FFF)`, which is fine only while
every target happens to be aligned. `tools/patch_rsprecomp.py` masks with
`0x1FFC`, restoring the hardware's behaviour for every microcode.

### The two channels arrive swapped

**Symptom:** correct-sounding music with the stereo image mirrored -- the kind of
bug that survives casual listening.

librecomp stores RDRAM byte-swapped so the `MEM_*` macros can read big-endian N64
words as native little-endian ones, which it does by XORing the low bits of every
address. `ultramodern::queue_audio_buffer` hands out a **raw pointer**, which
skips that: within each 32-bit word the two 16-bit halves sit in the opposite
order to the cartridge's, and each word holds one left and one right sample. Swap
them back when queueing.

### Use the queue API, and do not let the sample rate drift

The interface ultramodern expects *is* a queue: the game asks how much is still
buffered and decides how much more to generate from the answer. SDL's
`SDL_QueueAudio` mirrors that directly; a pull callback needs its own ring buffer
in between and a second place for the sample count to drift.

Open the device with `SDL_AUDIO_ALLOW_ANY_CHANGE` **unset**, so SDL converts
internally if the hardware disagrees. The sample rate is the one thing that must
not silently differ: the game paces itself against how fast the queue drains, so
a device at 48000 while the game believes it is feeding 32000 runs the whole
audio thread at the wrong speed. Reopen the device when the game calls
`set_frequency`.

With no device at all, report the queue permanently drained. That keeps the
game's audio thread running at its normal cadence instead of stalling on a buffer
that never empties -- which is also the right shape for a deliberate silent stub.

### Give the microcode a private copy of its command list

**Symptom:** a click every ~30 seconds; occasional "audio frame dropped".

A game that double-buffers its audio command lists is assuming the RSP finishes a
task long before that buffer's turn comes round again. If a task runs late (see
"Build RelWithDebInfo" above), the game rewrites the list while the microcode is
still DMAing it in, `0x140` bytes at a time, and the microcode executes a
**splice of two frames' commands**. In this game one splice in particular -- an
ENVMIXER stripped of its own SETBUFFs, inheriting the frame-end SAVEBUFF's
`0x200` count -- walked a channel buffer off the end of DMEM and wrapped onto the
command jump table at `0x10`, so the *next* command jumped to a corrupted
address.

Copy the list to private scratch and point the task at the copy before running
it. The microcode reads the list's address exactly once, from the OSTask the
runtime places at DMEM `0xFC0`, so redirecting that one word is all it takes; a
raw copy between 8-byte-aligned addresses preserves RDRAM's byte swizzling. Cost
is a memcpy of a few KB per frame, and the outcome no longer depends on how late
the task runs.

Keep comparing the game's own buffer before and after the run and reporting when
it changed. It is a health metric now, and it is the measurement that found this.

### How the bisection was made to work

Re-running a failed task from a fresh DMEM with 1, 2, 3... of its commands and
comparing DMEM against a pristine copy after each finds the first command that
does damage. **Compare only the jump table itself** (`0x10`-`0x2F`), not the
surrounding window: the rest of that region holds DMA chunk-remainder bookkeeping
that legitimately varies with how many bytes of the list were consumed, and
comparing it flagged innocent commands (a plain SETBUFF, a RESAMPLE) in turn.
Narrowing the comparison was the fix needed before the tool could be trusted.

---

## 7. Renderer and presentation

The port's visual work is done in two places: patches to RT64
(`tools/patch_rt64.py`), and a **display-list rewriter** in the port
(`src/dlrewrite.cpp`) which inserts RT64's extended GBI commands into the game's
lists before RT64 sees them. The game's code and data are untouched.

### The display-list rewriter

The technique generalises to any RT64-based port whose game predates the extended
GBI:

- RT64 honours extended commands in any list that starts with `gEXEnable`, and
  its Fast3D microcode leaves the opcode free. **RT64 forgets the extended GBI at
  the end of every list**, so every list that uses it must enable it first.
- Copy each graphics task's list into scratch RDRAM, walk the top level, and
  insert commands where needed. Follow branches in place and leave calls as
  calls -- the called lists live where they live and RT64 runs them from there;
  scan a called list read-only to learn its extent and texture so it can be
  classified like an inline draw.
- Track the segment table (`G_MOVEWORD` / segment), the projection matrix and a
  modelview stack while walking, so matrices and vertices can be read from RDRAM
  and decisions made on what is actually there. RT64's own perspective test is
  element `[3][3]` of the projection.
- **Memory layout:** the runtime stores RDRAM 32-bit words natively, so a command
  is two host words at its physical offset. The 16-bit halves of a matrix or a
  vertex are the exception -- they sit at their offset **XOR 2**, which is also
  how RT64 reads them. A fixed-point `Mtx` is sixteen integer halves then sixteen
  fraction halves, row-major.
- Put the scratch buffer where the game cannot reach: a 4 MB cartridge on a
  runtime reporting 8 MB leaves the whole upper half free.
- **Cache what a called list draws by segmented *and* physical address.**
  Scanning a called list to learn what it draws is worth doing once and
  remembering, because the game's drawing lists are static and called by the same
  address every frame. But a segmented address says which segment a list is in,
  not where that segment is, and the game remaps segments between courses and
  between screens. Key the answer on the pair, so a remap misses the cache and
  rescans instead of answering for a list that is no longer there. Keying on the
  segmented address alone is the kind of fault that survives all testing on one
  course and appears on another.

### The four RT64 patches, and why each was needed

1. **The main-frame test.** RT64 decides whether a framebuffer is the game's main
   4:3 frame by comparing the scissor's shape to 4:3 within 10%. A game drawing
   into an inset region (303x199 here, 14% off) fails, and its 2D content is then
   stretched across the widened frame instead of kept at its original shape.
   The patch also accepts a scissor covering three quarters of the framebuffer's
   width and height, whatever its shape.
2. **The content crop.** The final blit maps the whole framebuffer to the window,
   so a game's own overscan borders are presented as black bars -- and a
   widescreen multiplier makes the side ones wider. The patch lets the port name
   the region the game draws into, and the blit fits that region to the window
   instead. Exposed through an `extern "C"` setter so the port needs no RT64
   headers, and off by default.
3. **Widening a 3D pass that covers the drawn region.** RT64 widens a 3D pass
   only when it reaches both edges of the frame it draws into. If anything else
   in the frame touches the full framebuffer, an inset world pass falls short and
   that frame alone renders at 4:3. The patch allows a sixteenth of the width in
   tolerance. **RT64 asks this question in two places** -- once to render the pass
   across the widened frame, once to widen the frustum that fills it -- and both
   must be patched: answering only the first stretches a 4:3 frustum across a
   wide viewport, which looks like a magnified image rather than a wider view.
4. **Interpolation measurement.** Nothing reports how well transform pairing is
   doing, so every change to interpolation had to be argued rather than measured.
   The patch counts, per frame: world transforms, how many found no pair, and how
   many of *those* had a matrix appearing nowhere in the previous frame. A port
   reads the running totals through an `extern "C"` accessor.

### Widescreen 2D: anchoring, stretching, and what not to stretch

RT64 anchors 2D geometry to a screen edge by the **origin carried on its
viewport**, and stretches by a flag on its **projection group** -- both per
projection rather than per triangle, so each change of class must reissue the
game's own viewport or projection command after the new alignment or group. Plain
rectangles have their own rect-alignment state, which applies to every rectangle
that follows and needs no command reissued.

Practical rules this port arrived at:

- **Cancel RT64's own origin displacement** so the game's viewport data can be
  reissued unchanged: RT64 displaces a viewport by `origin / 1024` of the
  framebuffer width, in quarter pixels, so subtract
  `origin * fbWidth * 4 / G_EX_ORIGIN_RIGHT`.
- **Inset anchors by how far the visible picture's edge lies inside RT64's
  widened frame**, or "anchored to the left edge" means RT64's edge rather than
  the window's, and the element sits off screen.
- **`G_EX_ASPECT_STRETCH` does not mean "cover the widened frame".** In RT64 it
  sets the rectangle's `aspectRatioScale` to 1, and that scale is what squeezes
  2D content back into the middle 4:3 of a widened framebuffer -- so the flag
  only says "do not squeeze this one". A rectangle the game draws across its own
  screen then covers the game's own columns of a framebuffer that is now wider,
  and stops short of the edges. What spreads an element across the frame is the
  **extended origins**, the mechanism anchoring already uses: a full-frame
  element anchors its left edge to the frame's left and its right edge to the
  frame's right (`convertViewportRect` places each edge relative to the origin it
  is given). Measured on a menu wipe drawn as four 96-pixel tiles: with the flag
  alone they landed at `x 0 w 96` and so on, unchanged; with both origins they
  landed at `x 0 w 441`, `441`, `882`, `1326` across a 1474-wide framebuffer.
  **Anchoring both edges is not on its own a fix, and it is not free.** Doing it
  for every stretched element placed them across the frame in the rectangle log
  and changed nothing that reached the screen, so something after placement is
  still deciding the width; and the same build began exiting cleanly of its own
  accord a minute or two into a run, which `WR64_HUD_OFF=1` stopped. Both are
  reverted. Whatever finally does this has to be checked over a whole run, not a
  frame.
- **Widen the scissor while anchoring**, and reissue the game's scissor command,
  or an anchored element is cut off at the frame's old edge.
- **When the pixels and the classifier disagree, ask the renderer.**
  `tools/patch_rt64_rectlog.py` makes RT64 print, under `WR64_RECT_LOG`, every
  rectangle's own coordinates, its origins and aspect flag, the framebuffer's
  width and scissor, and where it landed. It answered in one run what a day of
  screenshots had not: where each rectangle is placed, and so whether a fault
  lies in the port's classification or in what happens to the draw afterwards.
- **2D projection groups must carry no interpolation.** The same projection is
  reissued several times per frame with different flags, and RT64 must never
  blend one with another.
- **Classify runs of rectangles, not single rectangles.** Consecutive rectangles
  on one row and close together (12 pixels here) are one element -- the digits of
  a time, the markers of a row -- and classifying them individually tears a
  number at a zone boundary.
- **Stretch only under an orthographic projection.** A full-frame rectangle drawn
  under a *perspective* projection belongs to the 3D pass, which is already at
  the frame's full width; stretching it magnifies the picture. This game's intro
  composes shots from full-frame rectangles under the world's own projection, and
  stretching them made the picture jump between its proper width and a magnified
  one from shot to shot.
- **Not every full-frame rectangle under a perspective projection belongs to the
  picture.** The rule that keeps the 3D pass's own rectangles from being
  stretched -- they are already at the frame's full width, so stretching
  magnifies the picture -- also catches overlays that the game happens to issue
  inside the 3D pass. This game's opening lays a sun glare over its shots that
  way: same projection, same extent, same textured rectangle as the picture
  itself, and nothing in the frame distinguishes them. It showed as glare over
  the middle 4:3 of a widescreen picture. Keep a small built-in table of such
  identities in the port, above whatever override file players get, so the fix
  ships rather than being rediscovered by everyone.
- **Put a rectangle state back immediately after the element you set it for.**
  Alignment and aspect apply to every rectangle that follows, and a classifier
  that works on the top-level list only ever sees the rectangles the top level
  issues: a called list's rectangles are emitted by the renderer running that
  list and never pass through it. So they inherit whatever was left set. That is
  invisible while the state changes only between elements the top level draws in
  sequence, and it appears the moment one element is tagged: tagging the glare
  stretched everything drawn after it as well, including a logo drawn from a
  called list. Restore the previous class as soon as the tagged run is emitted
  rather than waiting for the next classified run.
- **Rewriting the world's frustum: copy the matrix, do not edit the game's.**
  A perspective projection arrives as a `G_MTX` pointing at a fixed-point `Mtx`
  the game owns and reuses; editing it in place corrupts the game's own data.
  Write a modified copy into scratch RDRAM and point the emitted `G_MTX` at that
  instead -- this port keeps a small area just past the one the rewritten list is
  built in. A field-of-view setting is then `m[0][0]` and `m[1][1]` divided by
  the ratio wanted, applied to **every** perspective pass of the frame, because
  the sky has a frustum of its own and comes apart from the world otherwise.
- **A per-frame arena's offsets are not identities.** The trick below works on a
  *fixed* table: the buoys' matrices come from segment 5 at `0xA1C0` every frame,
  so that offset greps straight to the function that fills it. The gate markers'
  matrices come from segment 3, which this game uses as a per-frame arena, and an
  offset there says only where the allocator landed in that frame on that course.
  It greps to nothing, and matching it against a trace of another course produced
  a confident, wrong answer. **Check that an address means the same thing twice
  before building on it** -- a display list in a static segment, or a texture
  image address, survives where an arena offset does not.
- **Finding a game's culling: search the recompiled C for the offset, not the
  threshold.** Draw distance is the game's own decision and a renderer cannot
  undo it, so it has to be found in the game's code -- in a project where the
  reference decompilation is still mostly assembly with `func_8xxxxxxx` names.
  What worked was not searching for the threshold, which turned out to be a
  variable rather than a constant, but searching for a **structural offset** that
  had already been observed at runtime: the trace showed the object's matrices
  coming from segment 5 at `+0xA1C0`, and `grep 0xA1C0 RecompiledFuncs/` found
  the one function that loads it. Reading outward from there gave the loop, the
  comparison and the address the limit is read from. **The recompiled C is a
  better search space than the disassembly** -- every instruction is there with
  its address in a comment, and it is one `grep` rather than a symbol hunt.
- **Then change the value, not the code.** The limit was a field in a struct the
  game keeps a pointer to, so the port writes it once per frame from the value
  the game itself last wrote -- scaling the game's number rather than replacing
  it, so a course that uses a different one keeps its proportions, and restoring
  it when the setting goes back to default. No patch to the recompiled code, and
  at the default setting nothing is written at all.
- **Two coincidences cost an hour each.** The measured cutoff was about 4,500 and
  `4500.0f` exists in the game's data; it is a coordinate. The value turned out
  to be 5,000 and a static table of course parameters holds 5,000; scaling every
  record in it changed nothing. Neither was the source. **A plausible constant in
  the data is not evidence** -- the test is whether changing it changes the
  behaviour.
- **Measure a far plane before offering a draw distance setting.** It is the
  obvious knob and it is often worth nothing: this game's far plane is already
  about twenty times further out than anything it draws (GAME-INTERNALS, *The
  world's frustum*), so a multiplier on it changes nothing. The test that settles
  it is to make the plane *smaller*, not larger -- if a quarter of the distance
  clips nothing, there is nothing out there to reveal.
- **A viewport's clip ratios defeat a "does this pass cover the frame" test.**
  RT64 measures a viewport as `translate +/- scale * clipRatio` (`rect()` in
  `rt64_rsp_viewport.h`), and the ratios are typically 3, so a full-width
  viewport measures three times the frame however far its centre has been moved.
  This game places a 3D object inside a 2D layout by moving a **full-size**
  viewport to the object's position (GAME-INTERNALS, *Placing a 3D object inside
  a 2D layout*), so every such pass answers "yes, I cover the frame", gets
  rendered across the widened frame, and the object lands at its 320-wide
  coordinate as a fraction of the *widened* width -- sliding outwards by exactly
  the widening factor while the 2D it belongs beside stays at 4:3. **Read the
  viewport's translate, not its measured extent.** A viewport the game has moved
  off centre is a placed object; give its pass an extended origin
  (`gEXSetViewportAlign` with `G_EX_ORIGIN_CENTER`), which is what switches the
  widening off for that projection and leaves it in the centred 4:3 region.
- **A test that reads the frame can still be fooled by a frame that looks like
  the thing it tests for.** This port decides "is this a race?" from the frame --
  world drawn first, under a perspective projection, into the inset scissor --
  precisely because the state variable was worse. The main menu draws exactly
  that way, so it answers yes, and the race HUD's anchoring rule then ran on a
  menu: it pinned each half of a symmetric pair to the edge it was nearer, and
  the cursor's two halves slid into the corners while the entry they belong to
  stayed centred. **Anchoring is by an element's centre past a third of the way
  out, which is a statement about a race HUD's layout and not about geometry.**
  Name the element in the tag table rather than loosening a frame test that earns
  its keep in a dozen states around a race.
- **An element classified one way in one frame and another in the next flickers**,
  however defensible each classification is. Report those as they happen and
  provide an override table keyed by texture or static-list address (this port
  reads `hud.json` from the settings folder), rather than tuning the heuristic
  until it happens to be stable.

### Interpolation: what RT64 can and cannot do on its own

RT64 draws frames between the game's by pairing each object's transform with the
previous frame's, by draw-call signature and nearest position. It pairs about 98%
of transforms. Two things follow:

- **An unpaired transform costs nothing by itself.** It is drawn at its current
  matrix, so an object that is not moving looks no different. The plain count of
  unpaired transforms is the wrong number to chase; the number worth watching is
  unpaired transforms whose matrix appears nowhere in the previous frame. Measured
  in a race here: 100-200 world transforms a frame, 2-3 unpaired, 1-2 of those
  moving or new. Whole-frame pairing failures do happen, but at scene cuts, where
  nothing should be interpolated anyway.
- **Geometry the game rebuilds every frame under an unchanging matrix is held
  still.** The matrix pairs perfectly, RT64 computes no motion, and the object
  steps at the game's rate while the world glides past it. This is the sky and the
  water in Wave Race 64, and it is the general case of "a game that recomputes a
  mesh rather than transforming it".

The fix is a matrix group asking for **vertex and texture-coordinate
interpolation**, which RT64 will do -- it takes the per-vertex difference from the
previous frame and carries it as a velocity -- but not by default: those
components are `G_EX_COMPONENT_SKIP` unless asked for, because most geometry that
changes its vertices between frames has been *replaced* rather than moved.

Before tagging a rebuilt mesh, check that index *i* means the same point in both
frames: same vertex count per block, and a stable reference vertex. Otherwise
interpolation translates the mesh rather than animating it.

**Check it over a run, not over a handful of frames, and with the camera
moving.** This is where this port got it wrong. The water passed the check across
four consecutive frames -- identical vertex counts, every block's first vertex
bit-for-bit identical -- and the check was taken as settled. Those four frames had
a stationary camera, which is the one condition under which a mesh built around
the camera is indistinguishable from a fixed one.

Measured over 2,172 race frames instead (`WR64_LATTICE`), the two meshes here
fail and pass in different ways, and the distinction is the useful part:

| | the water | the sky |
|---|---|---|
| moves in X or Z | 74% of frames | 95% of frames |
| all blocks move by the **same** step | **100%** of frames | 5% of frames |
| step of 60 world units or more | 31% of frames | 37% of frames |
| what it is | one rigid mesh, carried whole | three bands, each moving on its own |
| index *i* means | the same slot, carrying the same detail | the same point of that band |
| pairing by index | correspondence fine, **positions unusable** | sound |

So there are two distinct failures, and "does index *i* mean the same thing" only
catches the first:

- **Slots re-assigned.** Correspondence is lost; blocks move by different
  amounts. Interpolating smears one part of the mesh into another.
- **Positions quantized.** Correspondence is kept -- every block moves by the
  same step -- but the positions are the camera's, rounded to a grid the camera
  does not move on: 32, 64, 96, 128 units, while the camera moves a few units a
  frame. Interpolating between two of them is a surge, not motion. The mesh's
  *detail* is worth interpolating; its positions are not.

The second is what the water is, and it is invisible to every check made of a
single frame: stable vertex counts, stable block count, a stable reference
vertex, all hold. Only the step distribution over a moving run shows it. The
treatment is to interpolate the heights and texture coordinates against the
previous surface **sampled at each current world XZ**, inside the renderer, with
the history rejected at camera cuts and scene changes -- and to leave the
positions where the game put them.
([GAME-INTERNALS.md §6](GAME-INTERNALS.md#6-graphics) has both meshes' numbers.)

Two further details:

- Use `G_EX_COMPONENT_INTERPOLATE` for texture coordinates, not
  `G_EX_COMPONENT_AUTO`: automatic means "only when the positions did not
  change", which is the pure texture scroll of a waterfall.
- **Give such a section an explicit transform id with linear ordering.**
  Identical matrices are exactly what ties a matcher that pairs by position, and
  a tie lost means the section quietly falls back to the game's rate for a frame.
  An explicit id is matched first and by identity, before the heuristic, and
  several transforms sharing one id pair up in submission order. Values need only
  be distinct from each other and from `G_EX_ID_IGNORE` (0) and `G_EX_ID_AUTO`
  (~0).
- **RT64 creates a world transform at the first vertex after a matrix load, not
  at the load.** So a group set before a call to a static list is the group that
  list's transform is created under -- which means a whole mesh drawn from one
  static list can be wrapped without threading anything through it.

### Window creation affects the aspect ratio

RT64 derives the aspect ratio it expands the game into from the **swap chain's**
dimensions. A window created at a 4:3 multiple gives it a 4:3 swap chain, and an
"expand to the display" setting then has nothing to expand into -- the game stays
pillarboxed however wide the display is. Open at the display's own size for
fullscreen. Windowed, use the largest whole multiple of the game's resolution
that fits: a hardcoded 2x of 640x480 is 1280x960, taller than a 1536x864 laptop
panel, and Windows then places the window partly off screen with no indication
anything is wrong.

---

### The HUD inspector: fixing a 2D element with it in front of you

**[docs/HUD-INSPECTOR.md](HUD-INSPECTOR.md) is the manual and the recipe** -- how
to use it, and how to put the same tool in another port. This section is why it
looks the way it does.

**F1** draws a window inside RT64's developer UI listing every 2D
element of the current frame -- its identity, its extent in the game's own
320x240 pixels, whether it was drawn under a perspective or an orthographic
matrix, whether it came from a rectangle command or a run of triangles, and the
class the rewriter gave it. Hovering a row outlines that element on the screen.
Each row's class is a dropdown, and choosing another applies it from the next
frame; **Save to hud.json** writes what has been chosen into the tag table, so it
survives a restart and can then be moved into the port's built-in table.

Why this exists rather than a trace: the previous instrument was a log read
afterwards and matched to a screenshot by eye, and a menu wipe lasts six tenths
of a second. Several confident conclusions drawn that way turned out to be about
a different frame than the picture. An element's identity is also not something a
picture can show, so pointing at the thing to be fixed was impossible.

Three pieces make it work, and the middle one is the part worth copying:

- **RT64 owns the ImGui context**, so the port cannot open a window of its own.
  `tools/patch_rt64_inspector.py` adds one function pointer,
  `extern "C" void (*RT64_PortInspectorHook)()`, called once per frame from
  `State::inspect()` with an ImGui frame already open. Null unless the port sets
  it, so upstream behaviour is unchanged.
- **Four things are gated on RT64's developer mode**, and all four sit between
  the F1 key and the window: whether RT64 installs its event filter at all
  (`usesWindowMessageFilter()`), whether it looks at F1, whether the keystroke
  creates the inspector, and whether `State::inspect()` draws anything. The port
  therefore turns developer mode on **unconditionally**, in both the frontend and
  the runtime-only build. Gating a debug menu on a build flag or an environment
  variable means nobody has it when they need it -- the person looking at a
  misplaced menu element is running the game they downloaded. It costs a null
  check per frame while the menu is closed, because RT64 creates its inspector
  on the keystroke and `State::inspect()` returns immediately without one.
- **Developer mode is a bundle**, and turning it on for everyone arms every key
  behind it. Audit them rather than inheriting them: F2 toggles ray tracing for
  the session with nothing in a menu saying so and nothing on screen to explain
  the change, so `tools/patch_rt64_inspector.py` removes its `case` from both
  key filters and the key passes through to the game. F3 (view RDRAM) and F4
  (texture replacements) are visibly reversible and were kept.
- **Two threads meet.** The classifier runs on the thread that submits display
  lists; the panel runs on the renderer's UI thread. The classifier fills a frame
  under construction and publishes it under a mutex at the end of the frame; the
  panel only ever reads the published one. The override table is read by the
  classifier on every element, so it is kept to a handful of entries and shares
  the same lock.

What it does **not** need to provide: pausing, and asking what drew a given
pixel. RT64's developer mode already pauses the game and keeps the paused frame
interactive, and right-clicking a pixel lists the draw calls under it. The panel
adds a **Hold this frame** checkbox for the other case -- keeping the list on one
frame while the game runs on, which is what an animated element wants.

The outline's geometry is the rewriter's arithmetic read forwards: the game draws
in 320x240, RT64 squeezes that into a 4:3 box in the middle of the window, an
extended origin pins one edge of an element to the matching edge of the widened
frame instead, and stretch spreads it across the whole frame. It is an
approximation -- a scissor can still cut an element short -- but it is accurate
enough to point with.

---

## 8. Diagnostics that earned their keep

Every one of these was written to answer a specific failure and then kept.

| Tool | What it answers |
|---|---|
| **Vectored exception handler + dbghelp** (`src/crash_handler.cpp`) | Turns "it exits" into `SysMain_GfxFullSync + 0xF5 at funcs_13.c:7873`. The recompiled code is linked into the executable, so without symbol resolution every crash reports the same unhelpful module. |
| **Lookup-miss hook** (`tools/patch_librecomp.py`) | A failed lookup prints only the address, then asserts and exits -- and an exit is not an exception, so the crash handler never sees it. The hook reports the *calling* function, source line and thread. It immediately contradicted a claim this project had made two commits earlier. |
| **Function-entry instrumentation** (`tools/instrument_funcs.py`) | Inserts a one-shot `printf` at named recompiled functions, or with `NAME@0xADDR` prints an RDRAM word on every call. Order answers "did it run"; a watched value answers "and did it stay valid". Safe only because re-running the recompiler erases the edits. |
| **Hang watchdog** | A microcode that spins forever faults nothing, prints nothing and returns nothing. After a deadline, a persistent thread suspends the stuck thread, samples its instruction pointer repeatedly and resolves the distinct addresses to source lines. Sample repeatedly, not once: with everything inlined, one sample usually names a helper rather than the loop. (A thread *per call* here was real overhead on the thread that has to keep pace with the game.) |
| **3D frame trace** (`WR64_3D_TRACE`) | Writes a few whole frames -- every matrix load, vertex load, call and triangle batch, with each vertex block's FNV-1a hash and clip-space extent. Two consecutive frames diffed against each other say which geometry the game **rebuilds** rather than moves, which is exactly the geometry RT64 cannot interpolate unaided. This is how the sky and water were found. |
| **Spaced 3D frames** (`WR64_3D_TRACE_EVERY`) | The same trace with its frames spread over the run rather than consecutive. Consecutive frames answer what the renderer can pair between them; spaced frames answer whether an object is submitted at all from a distance, which is the question behind every report of things popping in. Diffing the display lists called in each frame separates "the game never submitted it" from "something downstream dropped it", and the modelview translation logged before each call gives the object's world position, so a culled set can be plotted rather than guessed at. |
| **Lattice trace** (`WR64_LATTICE`) | Whether a mesh the game rebuilds every frame can be paired between frames at all. For each frame, and for each rebuilt mesh (the water through segment 3, the sky through segment 6), it writes how many vertex blocks moved in X or Z since the previous frame, how many moved only in height, the largest step in each, and **how many moved by the same step as the first** -- which is what separates a mesh being carried whole from one re-assigning its slots. A handful of frames of identical vertices proves nothing: with the camera parked, a mesh built around the camera is indistinguishable from a fixed one. |
| **Rectangle log** (`WR64_RECT_LOG`, via `tools/patch_rt64_rectlog.py`) | Where every rectangle actually lands on the widened framebuffer, in RT64's own arithmetic: the game's coordinates, the origins and aspect flag the port set, the framebuffer width and scissor, and the resulting position. The port can say what it emitted and a screenshot can say what showed; only this says what the renderer did in between. |
| **Race trace** (`WR64_HAPTICS_TRACE`) | Every frame of every race as a CSV row -- speed, vertical velocity, airborne, wetness, impact, lap, buoy, misses, power, countdown -- with the feedback events each frame produced. Written to check that a set of RDRAM addresses really means what it is supposed to: the countdown counts down, speed rises under throttle, misses appear where the HUD says MISS. A value that looks plausible in one frame is not evidence; a column that behaves across a race is. |
| **Audio queue statistics** (`WR64_AUDIO_STATS`) | Whether the crackle is samples arriving late. Every two seconds: how many buffers the game produced and how big they were, the queue depth at its trough and average, the device period they have to cover, and -- the number it exists for -- how many frames of silence SDL had to insert. The device consumes at the sample rate whether or not anything is queued, so `elapsed x rate` minus the frames handed over (less the change in queue depth) is the shortfall, measured rather than inferred. It also prints what the machine's default device actually runs at, which `SDL_OpenAudioDevice` hides when `SDL_AUDIO_ALLOW_ANY_CHANGE` is unset: the spec it hands back mirrors the request no matter what the hardware does. |
| **Audio dump** (`WR64_AUDIO_DUMP`) | Whether the crackle is in the samples themselves. Writes exactly the bytes handed to `SDL_QueueAudio` to a WAV, one file per device open so the rate in its header is always right. This is the cut that halves the problem: everything that could corrupt samples -- the recompiled microcode, the command list, the channel swap, the volume scale -- is upstream of that call and everything that could deliver them late is downstream, and the two sound identical on a speaker. A clean file exonerates the first half outright. |
| **HUD inspector** (**F1**, via `tools/patch_rt64_inspector.py`) | Which 2D elements a frame contains, what identity each has, what class the rewriter gave it, and what happens if that class is changed -- answered while the frame is on the screen rather than in a log read afterwards. Hovering a row outlines the element; a dropdown changes its class from the next frame; a button writes the result into `hud.json`. See §7. |
| **2D draw trace** (`WR64_HUD_TRACE`) | Prints every 2D draw with its identity, extent and assigned class, and reports elements whose class changes between frames. |
| **State watcher and input scripts** (`src/testdrive.cpp`) | A port stuck on the title screen and one quietly racing look identical from outside. Watching the game's state variable produces a transcript -- title, menu, rider select, racing -- and an optional file of timed inputs makes a session repeatable and commitable. |
| **Window capture** (`tools/capture_window.ps1`) | The transcript says which screen the game thinks it is on; only a photograph says whether it is drawn correctly. |
| **Bisect switches** | `WR64_SKIP_DL` (skip RT64's display-list processing), `WR64_NO_REWRITE`, `WR64_HUD_OFF`, `WR64_NO_SKY_INTERP`, `WR64_NO_WATER_INTERP`. Whether a change is an improvement is often a question only a side-by-side can answer, and each switch also isolates a fault to one subsystem. |

---

## 9. Measurement traps

Small things, each of which cost real time here.

- **`cmd /c "prog & echo %errorlevel%"` reports the wrong value.** cmd expands
  `%errorlevel%` when it parses the line, before the program runs, so a hard
  crash reads back as a clean `EXITCODE=0`. An access violation was recorded as
  an orderly shutdown and the investigation went looking for who had called
  `quit()`. Use `cmd /v:on` with `!errorlevel!`, or run the program from
  PowerShell and read `$LASTEXITCODE`. (`Start-Process -PassThru` returned an
  empty `ExitCode` here.)
- **Unbuffer stdout.** Redirecting output to a file makes stdout fully buffered,
  and a process that is killed rather than exiting discards the buffer. The
  recompiled microcode reports unhandled jump targets through `printf`, and those
  reports were being lost entirely.
- **Send failed assertions to stderr, not a message box.** The debug CRT's modal
  dialog blocks the thread that raised it -- and that thread may be the one
  running RSP tasks, so the game freezes exactly as if the microcode had hung,
  with the explanation sitting in a window behind everything else.
- **Read the fault address relative to the RDRAM base.** `MEM_W` does no masking:
  it adds `0x80000000` to a register already holding a KSEG0 address, cancelling
  it out. So `rdram + 0x80000004` for `MEM_W(0x4, reg)` means `reg` was zero --
  the faulting address identifies a null pointer dereference on its own. Print the
  RDRAM base at startup so a crash report is interpretable at all.
- **A fault in the runtime is usually the runtime working.** librecomp allocates
  RDRAM inside a much larger `PAGE_NOACCESS` region precisely so an invalid game
  pointer faults immediately instead of silently corrupting memory.
- **A few frames with the camera parked is not a measurement of a moving game.**
  Four consecutive frames said this game's water lattice was fixed, and the port
  interpolated its vertices by index on that basis through a release. Over 2,172
  race frames it moves in 74% of them, in steps of 32 to 128 world units.
  Whatever a capture is meant to establish, take it while the thing it is about
  is happening -- a camera at rest hides everything that follows the camera.
- **`head` on a search is not the whole answer.** A truncated grep supported a
  confident claim about "the single caller" of an address. There were 19 call
  sites across two functions.
- **Do not overfit a scan.** A delta-sweep over a ±0x800 window can "explain" any
  address; an early version of this project's revision analysis reported 55
  regions covering 100% of symbols, which was an artifact. Only long runs are
  evidence -- a run of 126 consecutive symbols agreeing on one offset is not
  something chance produces, and a 6-symbol run is noise.

---

## Keeping this current

This file describes the *port*: the toolchain, the runtime, the renderer, and the
techniques used against them. When a later change fixes something in one of those
-- or finds that something written here is no longer true of a newer submodule --
update this file in the same commit that makes the change, and say what the
symptom was. A finding without its symptom is much harder to find again.

Facts about Wave Race 64 itself belong in
[GAME-INTERNALS.md](GAME-INTERNALS.md).
