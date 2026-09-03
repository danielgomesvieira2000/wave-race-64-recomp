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

## Configure and build

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

Apply the project's patch to RT64 first (see *The game's own black borders*
below for what it does and why it is a script):

```
python tools/patch_rt64.py
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

```
build-rt/WaveRace64Recomp.exe <your dump>.z64
```

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
Vertically it fits exactly; horizontally the widened frame has more picture
than the region, and the window shows as much of it as its shape allows -- the
frame a wider CRT would have shown, with nothing black around it. The HUD sits
at 4:3 inside that, where the cartridge put it.

**HUD Placement** in the Graphics tab does nothing for this game. It moves 2D
content that names an edge through RT64's extended GBI, and the cartridge
predates that.
