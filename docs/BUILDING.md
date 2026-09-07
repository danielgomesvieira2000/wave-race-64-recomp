# Building

## Requirements

| Tool | Version | Why |
|---|---|---|
| Git | any recent | submodules |
| CMake | 3.20+ | required by N64Recomp and the runtime |
| Ninja | any | the generator every upstream project assumes |
| Clang | 15+ | N64Recomp's output is validated against Clang and MSVC |
| Python | 3.10+ | splat and the symbol tooling in `tools/` |
| MIPS binutils | any | assembling the disassembly into an ELF (phase 01) |

**Do not build with modern GCC.** Recompiled output has a documented history of
miscompiling under modern GCC's optimizer. CMake will warn if you try.

### Windows

Verify what you have:

```powershell
pwsh -File tools/check_toolchain.ps1
```

Install what you don't (run in an elevated terminal):

```powershell
winget install --id Kitware.CMake
winget install --id Ninja-build.Ninja
winget install --id LLVM.LLVM
winget install --id Python.Python.3.12
winget install --id Microsoft.VisualStudio.2022.BuildTools `
    --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

The Build Tools install supplies the Windows SDK and the C runtime that Clang
links against on Windows; Clang alone is not enough.

A MIPS assembler is not available through winget. Phase 01 needs one of:

- **WSL** (`sudo apt install binutils-mips-linux-gnu`) — simplest, and what most
  decompilation projects assume.
- A prebuilt `mips64-elf` toolchain on `PATH`.

The Microsoft Store `python` stub on a fresh Windows install is not Python. If
`python --version` opens the Store, install the real thing with the winget
command above.

## Clone

```
git clone --recurse-submodules <this repo>
```

If you already cloned without submodules:

```
git submodule update --init --recursive
```

## The reference decompilation

The disassembly step runs inside a checkout of the Wave Race 64 decompilation
by LLONSIT and contributors, whose splat configuration, symbol tables and
linker scripts it uses. Clone it into `reference/` (which is ignored) and put
your dump beside its config under the name it expects:

```
git clone https://github.com/LLONSIT/Wave-Race-64.git reference/wr64-decomp
copy your.z64 reference\wr64-decomp\baserom.us.rev1.z64
```

Nothing from that checkout is committed here. The splat config this project
runs, `recomp/wr64.us.rev1.asm.yaml`, is derived from the decompilation's at
build time by `tools/make_asm_only_yaml.py` and is ignored as well; the
pipeline scripts generate it when it is missing.

## Configure and build

### Full Windows build with HD textures, modern water and replacement music

After generating `RecompiledFuncs/` **including `aspMain_rsp.cpp`**, run this
from PowerShell in the repository:

```powershell
pwsh -File tools/build_windows.ps1 -Package
```

The script locates Visual Studio (preferring 2022), loads its x64 SDK/toolchain,
uses `clang-cl`, applies the required patches and builds with all three runtime
options enabled. It validates and bundles all 1,828 HD texture mappings and nine
replacement recordings. High water, Aqua appearance, HD textures and Custom
music are the first-run defaults. The executable is
`build-windows/WaveRace64Recomp.exe`; the ZIP is under `dist/`.
WSL is only a build-time dependency; the finished game runs natively on Windows.

For the first source generation, install these dependencies inside Ubuntu WSL:

```sh
sudo apt-get update
sudo apt-get install cmake ninja-build clang binutils-mips-linux-gnu python3-venv python3-dev build-essential
```

In WSL, change to this repository's directory (for example,
`/mnt/e/Github/wave-race-64-recomp`). Initialize its submodules and clone the
reference decompilation as described above. The validated reference revision is
`a51b38a2aaef68da10ea1e47247e70be3b1d4c70`. Then run:

```sh
python3 -m venv "$HOME/wr64venv"
"$HOME/wr64venv/bin/pip" install PyYAML==6.0.3 pylibyaml==0.1.0 tqdm==4.67.1 \
    intervaltree==3.1.0 colorama==0.4.6 spimdisasm==1.42.4 rabbitizer==1.16.2 \
    pygfxd==1.0.5 n64img==0.3.3 crunch64==0.6.2
python3 tools/patch_n64recomp.py
python3 tools/patch_rsprecomp.py
cmake -S lib/N64ModernRuntime/N64Recomp -B build-tools -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-tools --target N64RecompCLI RSPRecomp -j 12
"$HOME/wr64venv/bin/python" tools/generate_game.py "/mnt/e/path/to/Wave Race 64 (USA) (Rev A).z64"
```

The generator verifies the ROM hash and the assembled code against the ROM
before recompiling both CPU and audio microcode. Return to PowerShell for
`build_windows.ps1`. Do not reuse a Windows CMake tool build directory for the
Linux recompiler or vice versa.

The replay runner also accepts a Windows executable or build directory:

```powershell
python tools/run_water_replay.py --app build-windows --quality high --course 1 --mode trials --through 1500 --output build-windows-qa/high
```

Windows replay reports leave the unsupported RSS measurement as `null`; the
stop tick, exit status, game state and renderer frame traces are still checked.

The Windows build was smoke-tested on an NVIDIA RTX 5090 using Direct3D 12,
60 Hz presentation and 4x MSAA. The Original and High replays exited cleanly;
all 49 gameplay checkpoints through tick 1410 matched. The latest High run used
the shipped HD/Custom/Aqua defaults and Events-only haptics, loaded all 1,828
texture mappings and nine recordings, and completed a 2,000-tick Sunny Beach
fixture with a clean exit. This is a startup/race smoke test, not a
complete playthrough of every mode or course. Vulkan shader compilation passes,
but this Windows run did not test Vulkan playback.

Two source-build fixes are included: the CPU recompiler skips graphics/audio
microcode labels emitted by splat's `textbin` segments (audio is compiled by
RSPRecomp), and the water graphics pipeline enables the input assembler in its
Direct3D 12 root signature. Without the latter, High water could compile but
crash when binding its first water pipeline.

### Individual build stages

The project is phase-gated so a partially finished tree always builds. Phase 00
needs no options:

```
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
cmake --build build
```

Later phases turn on what they earn:

| Option | Turned on by | Effect |
|---|---|---|
| `WR64_BUILD_RECOMPILER` | phase 02 | builds the N64Recomp tool from `lib/` |
| `WR64_WITH_RECOMPILED` | phase 02 | compiles the generated `RecompiledFuncs/` |
| `WR64_WITH_RUNTIME` | phase 03 | links ultramodern, librecomp and RT64 |

## Verify your dump

```
./build/WaveRace64Recomp --identify path/to/your.z64
```

Exit code 0 means the dump matches the pinned target. Anything else means stop
and fix that before continuing — a mismatched revision produces failures later
that look like tooling bugs.

## Known issues

### CMake 4.x and old vendored dependencies

CMake 4 removed compatibility with `cmake_minimum_required(VERSION <3.5)` and
hard-errors on it. Six files under `lib/RT64/src/contrib` still declare a lower
minimum:

```
imgui/examples/example_glfw_vulkan   2.8
implot/.github                       3.0
mupen64plus-win32-deps/SDL2*/cmake   3.0
re-spirv/external/SPIRV-Headers/...  3.0
```

All six are example, CI, or `find_package` config files that RT64's build does
not normally descend into, so this is expected to be harmless. If configure
fails naming one of them, the workaround is:

```
cmake -B build -G Ninja -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ...
```

Do not "fix" this by editing the submodules.

### Clang is not added to PATH by its installer

`winget install LLVM.LLVM` installs to `C:\Program Files\LLVM\bin` without
registering it. Add that directory to your user PATH.

## Building with the runtime (phase 03 onward)

Apply the project's runtime patches first, in this order. The water patch
supplies required renderer headers even when Original water is selected, and
the shutdown patch fixes the runtime queue teardown. These scripts are
idempotent and preserve the pinned upstream submodule revisions:

```
python tools/patch_rt64.py
python tools/patch_n64recomp.py
python tools/patch_rsprecomp.py
python tools/patch_librecomp.py
python tools/patch_water.py
python tools/patch_runtime_shutdown.py
```

Use **clang-cl**, not `clang++`, once `WR64_WITH_RUNTIME=ON`:

```
cmake -B build-rt -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl       -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWR64_WITH_RUNTIME=ON -DWR64_WITH_RECOMPILED=ON
cmake --build build-rt
```

**Always build optimized** (`RelWithDebInfo`: optimized, with the symbols the
crash handler needs). The project defaults to it when no build type is given,
but a build type already in a directory's cache wins over that default, and a
Debug build of this port is not merely slow -- it breaks the audio. The
recompiled audio microcode is run on a thread of its own, once per frame, and
the game double-buffers its command lists on the assumption that the RSP is
done with a list long before that buffer's turn comes round again, two frames
later. Unoptimized, one task takes 5 to 24 ms (measured) against a 16.7 ms
frame, so the game regularly starts rewriting a list the microcode is still
reading; the microcode then sees a splice of two frames' commands, and one
particular splice -- an ENVMIXER inheriting the frame-end SAVEBUFF's sample
count -- walks a buffer off the end of DMEM onto the command jump table. That
is the "audio frame dropped" click. Optimized, a task takes well under a
millisecond and the window closes. See docs/PHASE05-FINDINGS.md.

RT64 decides its warning flags from `CMAKE_CXX_SIMULATE_ID`: a Clang targeting
the MSVC ABI gets `/W4`, on the assumption that such a Clang is `clang-cl`. That
assumption is wrong for `clang++`, which targets the same ABI through the GNU
driver and rejects `/W4` outright, so the whole of RT64 fails to compile. Using
the driver RT64 expects is simpler than patching the submodule.

Changing compiler invalidates the CMake cache, and the options do not survive
it. Delete the build directory when switching rather than reconfiguring in
place, or the build will silently come back with the runtime switched off.

## Recompiling the audio microcode (phase 05 onward)

The game's audio is mixed by the RSP, so the port needs the cartridge's audio
microcode recompiled alongside the game code. That is a second tool with its own
config, run once after the recompiler is built:

```
python tools/patch_rsprecomp.py
wsl bash tools/wsl_build_recompiler.sh
wsl bash tools/wsl_recompile_rsp.sh
```

This writes `RecompiledFuncs/aspMain_rsp.cpp`, which the CMake build picks up
automatically and compiles with SSSE3 and SSE4.1 enabled -- librecomp's RSP
vector unit is written against those intrinsics. Like everything else under
`RecompiledFuncs/`, the file is derived from your dump and is not committed.

Without it the port still runs; it is simply silent, because the RSP callback
falls back to reporting each audio task complete without running it.

## Playing it

Double-click `build-fe\WaveRace64Recomp.exe` and pick your dump in the
launcher; it is remembered. From a shell, a path skips the launcher:

```
build-fe/WaveRace64Recomp.exe <your dump>.z64
```

Started by double-click, everything the port prints goes to `wr64.log` in the
settings directory below, and no console window opens. Started from a shell it
prints to the shell as before, so redirecting stderr to a file still works.

It can be started from any directory, by double-click or from a shell, and a
relative path to the dump is taken relative to where you typed it. Settings,
controller profiles and mod state live in `%LOCALAPPDATA%\WaveRace64Recomp`
on Windows (`$XDG_DATA_HOME/WaveRace64Recomp` or `~/.local/share/WaveRace64Recomp`
elsewhere), beside the files RT64 keeps there; the first line the port prints
says where. A file called `portable.txt` next to the executable keeps them next
to the executable instead.

A gamepad is used if one is attached. On the keyboard: arrow keys are the analog
stick, `X` is A, `C` is B, `Z` is Z, `Enter` is Start, `A` and `S` are the
shoulder buttons, `I`/`J`/`K`/`L` are the C buttons (the camera) and
`T`/`F`/`G`/`H` are the D-pad.

The window is sized to the largest whole multiple of 320x240 that fits the
display, and is resizable.

Two things help when reporting a problem. The port prints a transcript of the
game's own state to stderr -- title screen, main menu, rider select, racing --
so redirecting stderr to a file says where it got to. And
`tools/capture_window.ps1` photographs the window at intervals:

```
powershell -ExecutionPolicy Bypass -File tools/capture_window.ps1 -OutDir shots -Count 12 -IntervalSeconds 5
```

For a repeatable session without touching the pad, `WR64_INPUT_SCRIPT` points at
a file of timed inputs; `tools/scripts/race.txt` drives the game from boot into
a race. Leave the variable unset and nothing is injected.

## The frontend UI (phase 06, in progress)

`lib/RecompFrontend` is the shared library every N64: Recompiled port uses for
the parts that are not game-specific: `recompinput` for controller mapping,
rebindable keys and per-device profiles, and `recompui` for the config and mod
menus, built on RmlUi and drawn through RT64. It is off by default:

```
cmake -B build-fe -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl       -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWR64_WITH_RUNTIME=ON -DWR64_WITH_RECOMPILED=ON -DWR64_WITH_FRONTEND=ON
cmake --build build-fe
```

Three things were needed to build it outside the tree it was written in, all in
this project's `CMakeLists.txt` rather than as submodule patches:

- `sdl2_SOURCE_DIR` points at the SDL2 that RT64 already vendors. The frontend
  expects the variable FetchContent would have set, and two SDL2s in one process
  is not a thing that ends well.
- `__PRFCHWINTRIN_H` is defined for the frontend's directory. SDL 2.26 works
  around an old Clang bug by defining `_m_prefetch`; current Clang has it as a
  builtin, so the workaround is now the error. RT64 defines this guard for its
  own targets and the frontend does not inherit it.
- RT64's `DXC` variable and its option lists are re-declared. `recompui`
  compiles its own HLSL through RT64's shader functions, which are global, but
  the variables they expand are set inside RT64's directory -- so from a sibling
  directory the shader command line begins with the `.hlsl` file and no
  compiler, produces nothing, and fails a step later on an empty file.

`patches/ui_funcs.h` exists because `recompui` includes it by a hardcoded
relative path out of the library and into the port. Upstream marks it "TODO:
Forced game includes".

Two globals are declared `extern` inside recompui and defined by the port, in
`src/frontend.cpp`: `supported_games` and the SDL `window`.

### What the port supplies, and what it does not

The menus look and behave the same across every N64: Recompiled port because
recompui carries the theme in code -- a palette of named colours with the same
defaults the other recomps use -- and its elements style themselves from it. A
port that writes its own colours into `assets/recomp.rcss` does not restyle
those elements, it competes with them. So that file supplies the font and
nothing else. To restyle the menus, set theme colours from C++ with
`recompui::theme::set_theme_color`.

The font family in the stylesheet is the name declared inside the `.ttf`, not
its filename: `LatoLatin-Regular.ttf` calls itself `LatoLatin`. A mismatch fails
silently and strangely -- every element lays out and draws in the right place,
with no text in any of them.

`recompui::programconfig::set_program_name` and `set_program_id` must both be
called before anything is built. The launcher throws from its constructor
otherwise, and the process dies with nothing on stderr but `abort() has been
called`.

### Assets the port owns

`assets/` holds what recompui asks the port for and does not ship itself:

- `recomp.rcss`, the stylesheet, which is the font rule and nothing else.
- `promptfont/promptfont.ttf`, the controller and keyboard glyphs recompui draws
  input prompts with. PromptFont by Yukari "Shinmera" Hafner, v1.10, SIL Open
  Font License 1.1. Committed rather than fetched during the build, because a
  build that needs the network is a build that fails without one. See
  `assets/promptfont/README.md`.
- `icons/`, eleven SVGs recompui loads by name: Caret, Cont, Keyboard,
  PlusKeyboard, Question, Quit, RecordBorder, RecordSpinner, Reset, Trash, X.
  A missing icon is easy to miss and confusing when noticed: the control that
  uses it is still laid out, focusable and clickable, and draws nothing.

The icons are drawn for this project rather than taken from another port. The
other N64: Recompiled ports are GPL-3.0 and this repository is MIT, so their
assets cannot simply be copied in without changing the licence of this one.

Lato and Noto Emoji are not in `assets/`: the build copies them out of the RmlUi
submodule, which this repository already has.

## Display resolution and aspect ratio

The port opens fullscreen at the display's own resolution and aspect ratio, and
the game is rendered widescreen rather than pillarboxed.

Most of that is RT64's, exposed through the Graphics tab: **Resolution: Auto**
renders at the window's true pixel size instead of upscaling 320x240, and
**Aspect Ratio: Expand** widens the frustum to the window's shape. Both are
already the library's defaults. Two things had to change here.

**Window Mode** defaults to Windowed upstream. It is set to Fullscreen on a
first run only -- after `finalize()` has loaded the player's settings, and only
when no `graphics.json` exists -- so choosing Windowed in the menu sticks.

**The window is created at the display's size when fullscreen.** This is the one
that actually made widescreen work. RT64 derives the aspect ratio it expands
into from the swap chain's dimensions, and the window was being created at a
whole multiple of 320x240 -- a 4:3 swap chain, into which "Expand" expands
nothing. The menu said Expand and the game stayed 4:3.

### The game's own black borders

Wave Race 64 never draws to its whole 320x240 framebuffer. Every frame --
title, attract, menus, racing -- is drawn inside the region from (8, 20) to
(311, 219), a 303x199 window with black borders around it that a CRT's overscan
was meant to hide. Nothing hides them on a modern display: presented as-is
they are a black bar across the top tenth of the picture and, once the frame is
widened, forty pixels down each side. The values are the scissor the game sets,
read from RT64 during the title screen, the attract sequence and a championship
race; all three agree (`include/wr64/display.h`).

Two things in RT64 go wrong because of that region, and `tools/patch_rt64.py`
patches both. **Run it before configuring** -- it is scripted because it
patches a submodule, and a submodule update would revert it silently:

```
python tools/patch_rt64.py
```

**The frame was treated as "not 4:3" and its HUD stretched.** RT64 widens the
3D frustum for widescreen but keeps 2D content at its original shape in the
middle of the frame. It decides whether a framebuffer is the game's main 4:3
frame by comparing the scissor's shape to 4:3, within 10%; 303x199 is 1.52,
14% off, so every HUD element was stretched across the widened frame instead,
and the speed readout ended up at the far right edge and cut off. The patch
also accepts a scissor covering three quarters of the framebuffer's width and
height as the main frame, whatever its shape.

**The borders were presented as black bars.** The final blit mapped the whole
framebuffer to the window. The patch lets the port name the region the game
draws into (`wr64::display::crop_to_content()`, called as the window is
created), and the blit then scales that region to fit the window instead.
The region is not the same in every mode: with two players the game draws
into (8, 12)-(311, 229), taller than the single-player region, and a crop
pinned to the latter cut the top and bottom off the split screen in 0.1. The
display-list rewriter (`src/dlrewrite.cpp`) now reads the scissor the game
sets each frame and moves the crop to follow it, once the region has held
for a few frames.
Vertically it fits exactly; horizontally the widened frame has more picture
than the region, and the window shows as much of it as its shape allows -- the
frame a wider CRT would have shown, with nothing black around it. The HUD sits
at 4:3 inside that, where the cartridge put it.

**A 3D pass that covers the drawn region is not always widened.** RT64 widens
a 3D pass only when it reaches both edges of the frame it draws into. This
game scissors its world to (8, 20)-(311, 219), and in most frames nothing else
touches the framebuffer, so the frame's scissor is that same region and the
test passes. In the championship's warm-up round something else in the frame
touches the whole 320x240 framebuffer: the frame's scissor becomes the whole
thing, the world falls eight pixels short at each end, and the warm-up alone
was rendered at 4:3. The patch allows a sixteenth of the frame's width in
tolerance, which is more than the border and far less than the inset boxes the
select screens draw their models into. RT64 asks the question in two places --
once to render the pass across the widened frame, once to widen the frustum
that fills it -- and both are patched; answering only the first stretches the
game's 4:3 frustum across a wide viewport, which looks like a stretched image
rather than a wider view.

**HUD Placement** in the Graphics tab does nothing for this game. It moves 2D
content that names an edge through RT64's extended GBI, and the cartridge
predates that.

## Frame rate

**Framerate** in the Graphics tab is RT64's interpolation, the same mechanism
the other recompiled ports use. *Original* shows the game's own frames.
*Display* presents at the display's refresh rate and *Manual* at a chosen rate,
capped at the display's; in both, RT64 draws the frames in between two game
frames by interpolating each object's transform -- position, rotation and scale
-- from one to the next. The game itself is untouched: Wave Race 64's physics,
camera and timers run at the rate the cartridge chose, and that rate varies.
The game writes a divider that its video-interrupt handler counts retraces
against: 3 in the menus and the attract demo (20 frames per second), 2 in a
race (30), 1 briefly at boot (60). RT64 measures it from the swaps and follows
it as it changes.

Nothing in the game had to be tagged for this. RT64 pairs each object's matrix
with the previous frame's on its own, by matching draw calls of the same
combiner, render mode and triangle count and then by nearest position, and in
the title, menus and attract demo it pairs about 98% of transforms per frame.
The remaining few are drawn where the newer frame puts them, so an object that
fails to pair judders at the game's rate against a smooth background rather than
smearing. Anything the game rebuilds every frame from vertices -- the water
surface's waves, the HUD -- animates at the game's rate; the camera's movement
over the water is smooth because the water is drawn in world space under the
interpolated view. Zelda 64: Recompiled tags every matrix with the actor it
belongs to from inside the game's code; that needs the drawing code to be
decompiled, and two thirds of this game's is not. If a specific object turns
out to pair badly, the port can rewrite the display list before RT64 sees it
and tag that object by hand; nothing so far has needed it.

**How frames are presented had to change for any of that to work.** The
frontend was handing RT64 the *Console* presentation mode: show the buffer the
N64's video interface would have shown, which for this triple-buffered game is
the one finished two frames earlier. RT64 only interpolates when the buffer it
has just drawn is the one being presented, which under Console never happens
for a game that buffers at all -- so the Framerate setting did nothing, however
it was set. The port now uses *PresentEarly*, as the other ports do: each frame
is shown as soon as it is drawn, two frames of latency go away, and
interpolation is possible. `WR64_PRESENT_MODE=console|skip|early` selects a
mode by name for comparing them; it is a testing knob, not a setting.

Two rates matter when something looks wrong, and the port prints both whenever
the first changes:

```
[wr64] the game is running at 30 frames per second; presenting at 60 (display 60 Hz)
```

The first is measured at `osViSwapBuffer`, which the game calls once per frame
it finishes (`patches/framerate.cpp`), so it says whether the machine is
keeping up with the game -- a race that keeps dropping from 30 to 20 says so
each time. The second is what the Framerate setting resolved to. With Display
selected on a 60 Hz panel the menus are presented at three times the game's
rate and a race at twice; a 144 Hz panel gets 144 either way.
