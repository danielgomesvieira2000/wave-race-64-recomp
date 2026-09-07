# Wave Race 64: Recompiled

> ## You need: Wave Race 64 (USA) (Rev A)
>
> Also called **v1.1**. Cartridge ID `WR`, region `E`, **revision `1`**,
> 8 MiB `.z64`, header CRC `0x492F4B61 0x04E5146A`,
> sha1 `508dfc2d4caa42b6f6de5263d0aed5e44ac7966a`.
>
> The original US release (v1.0, revision 0), the Japanese and European
> versions and the Shindou edition **will not work**: their code is laid out
> differently, and every address in this project is tied to Rev A. The
> launcher checks the file you pick; from a shell,
> `WaveRace64Recomp.exe --identify your.z64` prints what it is.
>
> Supply your own legally obtained dump. This project does not include one,
> and never will.

A native PC port of Wave Race 64, made by statically recompiling the game's
MIPS code to C with [N64Recomp](https://github.com/N64Recomp/N64Recomp) and
running it on [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime)
with [RT64](https://github.com/rt64/rt64) as the renderer and
[RecompFrontend](https://github.com/N64Recomp/RecompFrontend) for the menus
and controller support. It is unofficial and not affiliated with Nintendo.

## Getting it

**Download.** The upstream release for Windows x64 is on the
[original project's Releases page](https://github.com/danielgomesvieira2000/wave-race-64-recomp/releases): unzip,
run `WaveRace64Recomp.exe`, pick your dump in the launcher. That is all. The
zip contains the program, the three DLLs it needs, and the menu's fonts and
icons; it contains none of the game's assets, which are loaded from your dump
each time it runs. What it does contain is the game's *code*, recompiled --
that is what a recompiled port is. See *Licensing* below before
redistributing it.

**Build it yourself.** [docs/BUILDING.md](docs/BUILDING.md) takes you from
installing the toolchain on Windows to the first race: clone with submodules,
check your dump, disassemble and recompile the game from it, build.
For a native Apple Silicon app using Metal, see [docs/MACOS.md](docs/MACOS.md).

Settings, controller profiles and saves live in
`%LOCALAPPDATA%\WaveRace64Recomp`. Started by double-click, the program writes
its log to `wr64.log` in that folder; attach that file to a bug report.

## What 0.3 is

The game boots, its menus work, and championship, time trial and two-player
races run with audio, at speed, with records saved to the emulated EEPROM.
Not every course has been played in both directions yet, and stunt mode and
the championship ceremony have had less testing than the rest; reports of
anything wrong there are welcome.

New in 0.3, all of it widescreen work on the 2D layer: the race HUD is laid
out across the frame rather than kept at 4:3 in the middle of it -- time
top-left, rank and lap centre, speed and the leaderboard right, MISS and power
along the bottom -- while menus keep the 4:3 layouts their frames are drawn
for. The game's full-screen overlays, the tint over a race and the pause
screen's dim, now reach the edges instead of leaving a brighter band down each
side. And the warm-up round is widescreen like every other race, truly so
rather than a stretched 4:3 picture.

From 0.2: the 3D models on the watercraft and rider select screens sit in
their frames in widescreen, and two-player split screen is presented in full.

All of it comes from a display-list rewriter in the port, which inserts RT64's
extended commands into the game's lists before RT64 sees them without touching
the game's code, and from four scripted patches to RT64 (see `docs/PLAN.md`,
phase 07, and `tools/patch_rt64.py`).

Beyond running natively, the port adds:

- **Widescreen** at the display's resolution and aspect ratio, with the game's
  own black overscan borders removed. **HUD Placement** in the Graphics tab
  chooses whether the race HUD is laid out across the frame or kept at 4:3 in
  the middle of it.
- **High frame rate.** The game keeps its own update rate (30 Hz in a race, 20
  in the menus and Time Trial, by its own choice) and RT64 interpolates each
  object's movement between game frames, so it presents at your display's
  refresh rate. Physics, camera and timers are untouched.
- A launcher with a ROM picker, a settings menu, and controller remapping with
  per-device profiles, from RecompFrontend.
- Audio through the recompiled RSP microcode.

## Modern water preview

[![Watch the modern water showcase on YouTube](https://i.ytimg.com/vi/ikUGbLmPbvA/hqdefault.jpg)](https://www.youtube.com/watch?v=ikUGbLmPbvA)

[Watch the water shader in motion on YouTube](https://www.youtube.com/watch?v=ikUGbLmPbvA).

The source build includes an optional water renderer. In **Graphics → Water**,
choose **Modern** for detailed ripples, sun/sky lighting, depth color,
refraction, shoreline wash and persistent wakes, or **High** to add screen-space reflections
and fine spray. **Original** remains the default. Press **F9** for a comparison
from the current camera; **F10** cycles the rendering diagnostics.

The **Water** tab adds **Modern / Classic** appearance and **Soft / Normal /
Strong** surface ripples. Classic preserves the original course colors,
transparency and broad wave highlights while adding reflections, wakes and
spray. Modern with Normal ripples keeps the earlier appearance. Both styles
use irregular foam breakup to avoid a square pattern behind the craft. Wave
heights and reflection highlights interpolate at the selected display rate,
including when the original water grid recenters around the camera. **Spray
particles → Off** removes the added airborne spray while retaining surface foam
and wakes. Spray close to the camera also fades out before it can fill the screen. Fine
ripples use irregular wind-stretched patterns to avoid repeating crossed bands.

The original wave mesh and game physics remain authoritative. Water assets
are procedural and course settings are in `assets/water/profiles.json`.
Metal is the runtime tested backend; SPIR-V and DXIL compilation is checked
separately. See [the implementation and evidence](docs/MODERN_WATER_PROGRESS.md)
for the acceptance matrix, scope and reproduction commands.

Known issues:

- In a 4:3 window the picture is letterboxed. The game draws a 303x199 region
  of its 320x240 framebuffer, which does not fit a 4:3 window without bars.
  Fullscreen on a widescreen display, the default, has no bars.
- The **results screen**'s layout is wrong when HUD Placement is set larger
  than Original.
- The **MAX POWER** banner loses its last letter while the HUD layout is on.
- Downloadable releases are Windows-only; native Apple Silicon builds are
  available from source (see the macOS build guide above).

## Layout

```
src/        the port: platform layer, renderer binding, input, audio, launcher wiring
include/    its headers
patches/    replacements and wrappers for individual game functions
recomp/     N64Recomp configuration
tools/      the build pipeline's scripts, and diagnostics (see tools/README.md)
assets/     the launcher's stylesheet, icons and fonts
docs/       the build guide, the phase plan, and what each phase found
lib/        upstream submodules
```

Everything derived from a dump -- the disassembly, `RecompiledFuncs/`, the
ELF -- is generated locally and refused by `.gitignore`. So is the splat
config, which is derived from the reference decompilation's.

## Licensing

The project's own code and artwork are under the MIT License (`LICENSE`).

A built executable is another matter. It links N64ModernRuntime statically,
and N64ModernRuntime is **GPL-3.0**, so the executable as a whole is a
GPL-3.0 combined work: anyone who distributes one must provide its complete
source under the GPL's terms, and this repository at the tagged commit,
with the submodules it pins, is that source. RecompFrontend, which supplies
the menus, has published **no license** at the time of this release. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the full list.

## How this was made

The original upstream project was written by Claude, Anthropic's AI model, working in
[Claude Code](https://claude.com/claude-code) under the direction of Daniel
Gomes Vieira, who set the goals, made the decisions, played the builds and
reported what he saw. The launcher artwork and the executable's icon were
generated with Claude as well. The work was done in phases (see
[docs/PLAN.md](docs/PLAN.md)), and each phase's findings are written up under
`docs/`, in the same way: by Claude, as the work was done.

This fork adds native macOS support and the optional modern water renderer,
developed with Codex under Brian Tate's direction and validated through native
playtesting and deterministic replays.

## Credits

The recompilation toolchain and runtime are by Mr-Wiseguy and the N64Recomp
contributors; RT64 is by Darío and the RT64 contributors. The function names
this project leans on come from the Wave Race 64 decompilation by LLONSIT and
contributors. Earlier recompilation attempts by WACOMalt and chronic8000 showed
what to expect.
