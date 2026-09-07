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

**macOS (Apple Silicon).** Download the ZIP from
[this fork's latest release](https://github.com/elliotttate/wave-race-64-recomp/releases/latest),
extract it, and drag `WaveRace64Recomp.app` to Applications. Requires macOS 15
or later and your USA Rev A dump. Libraries are bundled; no build tools are
needed. See [installation and first-launch instructions](docs/MACOS_RELEASE.md).

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

## Replacement soundtrack

Source builds of this fork include nine replacement recordings: Bryan EL's
Main Theme and Dolphin Park, plus seven tracks from Retro Game Remix's
*Dolphin Park* album. The build bundles the recordings, loop settings, and
[artist credits](assets/music/CREDITS.md). They are available under
**Settings → Sound → Music → Custom**, with a separate custom-music volume
control. **Custom is enabled by default.** **Original** restores the cartridge
soundtrack.

All courses have replacements; Options and first-place results also have
assigned recordings. Effects and announcer commands keep using the original
audio. Local recordings can override individual bundled tracks. See
[the track mapping and setup guide](docs/MUSIC.md). These additions follow the
`v0.4.0-macos.1` binary release and require a build of the current source.

## Bundled HD textures

This fork includes the reviewed [HD texture pack](assets/textures/nano-banana-2),
with **1,828 replacement mappings**. **HD is enabled by default** under
**Settings → Graphics → Textures**; choose Original to use the cartridge artwork.
The build bundles the textures alongside the music, so no separate pack install
is needed.

The pack includes refined HUD fonts and signs, both Wave Race logos, eight
helmet portraits with dark visors, sharper rank and buoy icons, ramp wood,
island materials, and improved jet-ski side panels for the primary and opponent
models. Most textures are 4×; the focused portraits, icons, terrain, wood and
secondary craft panels use reviewed 8× replacements. World materials have
complete mip chains. Rejected generated candidates retain source-preserving
replacements, so coverage does not imply that every material gained new detail.
See [credits](assets/textures/nano-banana-2/CREDITS.md) and
[coverage and reconstruction details](docs/HD_TEXTURES.md).

## Modern water preview

[![Watch the modern water showcase on YouTube](https://i.ytimg.com/vi/ikUGbLmPbvA/hqdefault.jpg)](https://www.youtube.com/watch?v=ikUGbLmPbvA)

[Watch the water shader in motion on YouTube](https://www.youtube.com/watch?v=ikUGbLmPbvA).

The source build enables **High water with Modern appearance by default**.
In **Graphics → Water**,
choose **Modern** for detailed ripples, sun/sky lighting, depth color,
refraction, shoreline wash and persistent wakes, or **High** to add screen-space reflections
and fine spray. Choose **Original** to restore original rendering. Press **F9** for a comparison
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
- The macOS download supports Apple Silicon; Intel Macs have not been validated.

## Layout

```
src/        the port: platform layer, renderer binding, input, audio, launcher wiring
include/    its headers
patches/    replacements and wrappers for individual game functions
recomp/     N64Recomp configuration
tools/      the build pipeline's scripts, and diagnostics (see tools/README.md)
assets/     launcher assets, replacement music, HD textures and water profiles
docs/       the build guide, the phase plan, and what each phase found
lib/        upstream submodules
```

ROM dumps, disassembly, `RecompiledFuncs/`, and the ELF are generated locally and
refused by `.gitignore`. So is the splat config derived from the reference
decompilation. The reviewed replacement artwork is included separately under
`assets/textures`; raw texture captures and generation work files remain local.

## Licensing

The project's own code and artwork are under the MIT License (`LICENSE`).
The separately credited recordings in `assets/music` and replacement artwork
in `assets/textures` are excluded from that license; the underlying recordings,
compositions and game artwork remain the work of their respective owners. See
[music credits](assets/music/CREDITS.md) and
[texture credits](assets/textures/nano-banana-2/CREDITS.md).

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

This fork adds native macOS support, modern water, replacement music and HD textures,
developed with Codex under Brian Tate's direction and validated through native
playtesting and deterministic replays.

## Credits

The recompilation toolchain and runtime are by Mr-Wiseguy and the N64Recomp
contributors; RT64 is by Darío and the RT64 contributors. The function names
this project leans on come from the Wave Race 64 decompilation by LLONSIT and
contributors. Earlier recompilation attempts by WACOMalt and chronic8000 showed
what to expect.
