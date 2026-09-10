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

**Download.** The release for Windows x64 is on the Releases page: unzip,
run `WaveRace64Recomp.exe`, pick your dump in the launcher. That is all. The
zip contains the program, the three DLLs it needs, and the menu's fonts and
icons; it contains none of the game's assets, which are loaded from your dump
each time it runs. What it does contain is the game's *code*, recompiled --
that is what a recompiled port is. See *Licensing* below before
redistributing it.

**Build it yourself.** [docs/BUILDING.md](docs/BUILDING.md) takes you from
installing the toolchain on Windows to the first race: clone with submodules,
check your dump, disassemble and recompile the game from it, build.

Settings, controller profiles and saves live in
`%LOCALAPPDATA%\WaveRace64Recomp`. Started by double-click, the program writes
its log to `wr64.log` in that folder; attach that file to a bug report.

## What 0.5 is

The game boots, its menus work, and championship, time trial and two-player
races run with audio, at speed, with records saved to the emulated EEPROM.
Not every course has been played in both directions yet, and stunt mode and
the championship ceremony have had less testing than the rest; reports of
anything wrong there are welcome.

New in 0.5, all of it about the controller and the sound. The pad rumbles in a
game that shipped a year before the Rumble Pak, worked out from the race itself:
landings off waves, collisions, buoys taken and missed, the countdown and the
flag. The Sound tab's **Main Volume** is applied for the first time -- it had
never done anything -- and **Music Volume** sits beside it, scaling the music
inside the game's own audio engine without touching the effects. The game goes
quiet when another window has focus. Pads are assigned as they are plugged in,
the keyboard works alongside them, and the remapping in the controls tab takes
effect at last. Steering is analogue again, where every deflection past the
deadzone used to clamp to full lock.

From 0.4: the sky and the water move with everything else at high frame rates,
and the opening keeps its proper width from shot to shot.

From 0.3, all of it widescreen work on the 2D layer: the race HUD is laid
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
- **Controller rumble**, which the game never had: it shipped a year before the
  Rumble Pak and has no rumble code at all, so the port works the feedback out
  from the race itself -- the slap of landing off a wave, buoys taken and missed,
  collisions, the countdown and the flag. **Rumble Strength** in the General tab
  sets how hard, and zero turns it off.
- A launcher with a ROM picker, a settings menu, and controller remapping with
  per-device profiles, from RecompFrontend. **Pads are assigned as they are
  plugged in** -- the first is player one, a second is player two -- so nothing
  has to be set up before playing.
- Audio through the recompiled RSP microcode, with the Sound tab's **Main
  Volume** applied to it.

Known issues:

- In a 4:3 window the picture is letterboxed. The game draws a 303x199 region
  of its 320x240 framebuffer, which does not fit a 4:3 window without bars.
  Fullscreen on a widescreen display, the default, has no bars.
- The **results screen**'s layout is wrong when HUD Placement is set larger
  than Original.
- The **MAX POWER** banner loses its last letter while the HUD layout is on.
- There is **no separate volume for the announcer**. Music and effects have their
  own sliders because the game keeps them on separate sequence players, and the
  voice was expected to be separable the same way; the channels that looked like
  it turned out to be a countdown sound that starts when the announcer does. See
  [docs/PORTING.md](docs/PORTING.md), *Separating one sound from another*.
- Windows only, for now.

## Layout

```
src/        the port: platform layer, renderer binding, input, audio, launcher wiring
include/    its headers
patches/    replacements and wrappers for individual game functions
recomp/     N64Recomp configuration
tools/      the build pipeline's scripts, and diagnostics (see tools/README.md)
assets/     the launcher's stylesheet, icons and fonts
docs/       the build guide, the technical reference, the phase plan and findings
lib/        upstream submodules
```

Everything derived from a dump -- the disassembly, `RecompiledFuncs/`, the
ELF -- is generated locally and refused by `.gitignore`. So is the splat
config, which is derived from the reference decompilation's.

## Technical documentation

Three documents write down what this port reverse engineered, ran into, or built
to see with, for anyone working on the Wave Race 64 decompilation or on another
N64 port. They are kept current as the port changes.

- **[docs/GAME-INTERNALS.md](docs/GAME-INTERNALS.md)** -- the game. Cartridge
  identity and how Rev A relates to v1.0, the code and overlay layout, the main
  loop's ordering, the game-state machine and its jump tables, the overlay table
  format and the two loading paths, how a frame's display list is built, the
  303x199 drawn region, how the sky and water geometry is rebuilt each frame,
  the video frame divider, and the audio microcode: its IMEM load address, its
  command jump table, its DMEM map and its command-list buffering.
- **[docs/PORTING.md](docs/PORTING.md)** -- the toolchain and runtime. Getting a
  byte-exact ELF out of splat, N64Recomp's input modes and its failure messages,
  overlay dispatch by runtime lookup and everything that breaks when you turn it
  on, the librecomp/ultramodern harness, recompiling RSP audio microcode, the
  RT64 patches and the display-list rewriter behind widescreen and interpolation,
  the diagnostics that were worth building, and the measurement traps that cost
  the most time.
- **[docs/HUD-INSPECTOR.md](docs/HUD-INSPECTOR.md)** -- the in-game inspector for
  the widescreen 2D layer: how to use it to find and fix an element while the
  game is running, and a five-piece recipe for putting the same tool in another
  N64 port on RT64.

The phase findings under `docs/` are the working record the first two were
distilled from; they keep the wrong turns, which are often the useful part.

## Contributing

[CONTRIBUTING.md](CONTRIBUTING.md) has the rules and the working conventions.
The short version: no copyrighted game material enters this repository -- no
dump, nothing extracted from one, and nothing derived from what was extracted,
upscaled textures included. Facts about the ROM (addresses, layouts, formats)
are what the documentation is made of and are welcome; the ROM's contents are
not.

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

The entire project -- every line of code, every script, every document
including this one -- was written by Claude, Anthropic's AI model, working in
[Claude Code](https://claude.com/claude-code) under the direction of Daniel
Gomes Vieira, who set the goals, made the decisions, played the builds and
reported what he saw. The launcher artwork and the executable's icon were
generated with Claude as well. The work was done in phases (see
[docs/PLAN.md](docs/PLAN.md)), and each phase's findings are written up under
`docs/`, in the same way: by Claude, as the work was done.

## Credits

The recompilation toolchain and runtime are by Mr-Wiseguy and the N64Recomp
contributors; RT64 is by Darío and the RT64 contributors. The function names
this project leans on come from the Wave Race 64 decompilation by LLONSIT and
contributors. Earlier recompilation attempts by WACOMalt and chronic8000 showed
what to expect.

**Controller rumble is built on work by [Elliott
Tate](https://github.com/elliotttate).** The addresses this port reads a race
from -- the craft's speed, its vertical velocity, whether it is airborne, how wet
the hull is, collisions, laps, buoys, misses, power, the countdown -- were
identified in his fork and offered to this project in
[pull request #2](https://github.com/danielgomesvieira2000/wave-race-64-recomp/pull/2),
where they drive a much larger feedback system than this one. They are
re-verified here against a scripted race, and recorded in
[docs/GAME-INTERNALS.md](docs/GAME-INTERNALS.md) §7 so the next port does not
have to find them again. That pull request also carries native macOS support and
a modern water renderer, neither of which is in this repository.
