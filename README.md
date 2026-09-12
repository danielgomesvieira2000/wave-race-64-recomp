# Wave Race 64: Recompiled

A native PC port of Wave Race 64, made by statically recompiling the game's MIPS
code to C with [N64Recomp](https://github.com/N64Recomp/N64Recomp) and running it
on [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) with
[RT64](https://github.com/rt64/rt64) as the renderer and
[RecompFrontend](https://github.com/N64Recomp/RecompFrontend) for the menus and
controller support.

Widescreen, high frame rate, rumble in a game that never had it, a debug menu on
F1, and mod support including texture packs. Unofficial, and not affiliated with
Nintendo.

> ### You need: Wave Race 64 (USA) (Rev A)
>
> Also called **v1.1**. Cartridge ID `WR`, region `E`, **revision `1`**,
> 8 MiB `.z64`, header CRC `0x492F4B61 0x04E5146A`,
> sha1 `508dfc2d4caa42b6f6de5263d0aed5e44ac7966a`.
>
> The original US release (v1.0, revision 0), the Japanese and European versions
> and the Shindou edition **will not work**: their code is laid out differently,
> and every address in this project is tied to Rev A. The launcher checks the
> file you pick; from a shell, `WaveRace64Recomp.exe --identify your.z64` prints
> what it is.
>
> Supply your own legally obtained dump. This project does not include one, and
> never will.

## Getting started

**Download.** Windows x64 and Linux x86-64 builds are on the
[Releases](../../releases) page. Unzip or untar, run it, pick your dump in the
launcher. That is all. Each archive contains the program, the libraries it
cannot start without, and the menu's fonts and icons; none contains any of the
game's assets, which are loaded from your dump each time it runs. What they do
contain is the game's *code*, recompiled -- that is what a recompiled port is.
See [Licensing](#licensing) before redistributing it.

There is **no macOS binary**: it builds, and is played by the person who wrote
that support, but nobody here has a Mac to build or sign one on. Build it
yourself with the two commands below.

**Build it yourself,** on Windows, Linux or macOS.
[docs/BUILDING.md](docs/BUILDING.md) takes you from installing the toolchain to
the first race. Linux and macOS have a script each that does the whole thing
from a clean clone:

```sh
bash tools/setup_linux.sh --install      # or tools/setup_macos.sh
bash tools/build_linux.sh "/path/to/Wave Race 64 (USA) (Rev A).z64"
```

Settings, saves and mods live in `%LOCALAPPDATA%\WaveRace64Recomp` on Windows,
`~/.local/share/WaveRace64Recomp` on Linux and
`~/Library/Application Support/WaveRace64Recomp` on macOS. Started by
double-click, the program writes its log to `wr64.log` there; attach that file
to a bug report.

## What it does

The game boots, its menus work, and championship, time trial and two-player races
run with audio, at speed, with records saved to the emulated EEPROM. Beyond
running natively:

- **Widescreen** at the display's resolution and aspect ratio, with the game's
  own black overscan borders removed. **HUD Placement** in the Graphics tab
  chooses whether the race HUD is laid out across the frame or kept at 4:3 in the
  middle of it.
- **High frame rate.** The game keeps its own update rate -- 30 Hz in a race, 20
  in the menus and Time Trial, by its own choice -- and RT64 interpolates each
  object's movement between game frames, so it presents at your display's refresh
  rate. Physics, camera and timers are untouched.
- **Field of View**, 45 to 110 degrees. The game draws at 45; higher shows more
  of the world without stretching anything.
- **Modern water**, in the Water tab, which keeps the cartridge's waves, physics
  and timing and changes only how that surface is shaded: sun and sky lighting,
  colour that deepens with the water, refraction, persistent wakes, shoreline
  wash, and on High reflections and airborne spray. It costs roughly double the
  time spent drawing a frame, so **Original** is there and is a true bypass.
  Written by [Elliott Tate](https://github.com/elliotttate); see
  [Credits](#credits).
- **Draw Distance**, up to 4x. There is no global draw distance in this game --
  the far plane is already twenty times further out than anything drawn, and each
  kind of object is culled by its own code. This is one setting over the limits
  that have been found; today it reaches the buoys, which the game drops at 5,000
  units while drawing the world to 16,192.
- **Controller rumble**, which the game never had: it shipped a year before the
  Rumble Pak and has no rumble code at all, so the port works the feedback out
  from the race itself -- the slap of landing off a wave, buoys taken and missed,
  collisions, the countdown and the flag. **Rumble Strength** in the General tab
  sets how hard, and zero turns it off.
- **Audio** through the recompiled RSP microcode, with **Main Volume**, **Music
  Volume** and **Mute When Not In Focus** in the Sound tab.
- **A launcher** with a ROM picker, settings, and controller remapping with
  per-device profiles, from RecompFrontend. **Pads are assigned as they are
  plugged in** -- the first is player one, a second is player two -- and the
  keyboard works alongside them, so nothing has to be set up before playing.

The widescreen work comes from a display-list rewriter in the port, which inserts
RT64's extended commands into the game's lists before RT64 sees them, without
touching the game's code -- plus a handful of scripted patches to RT64 itself.
See [docs/PORTING.md](docs/PORTING.md).

[CHANGELOG.md](CHANGELOG.md) has what changed in each release.

## Mods

The **Mods** tab installs, enables and reorders mods, and the launcher has an
entry for it. A mod is a `.nrm` -- a zip with a `mod.json` -- and can carry:

| Content | Declared by | Handled by |
|---|---|---|
| **Texture pack** | `rt64.json` | this port, through RT64's replacement system |
| Recompiled code | `mod_binary.bin` + `mod_syms.bin` | the runtime, recompiled live at load |
| ROM patch | `patch.bps` | the runtime |

Texture packs are read straight out of the mod file, apply in the order the tab
shows, and can be switched on and off without restarting.
[examples/mods/](examples/mods) has a working template, `tools/pack_mod.py`
builds a `.nrm` from a directory, and
[examples/mods/README.md](examples/mods/README.md) has the workflow -- including
the part that matters here: ship your own images, never the game's textures or
anything derived from them.

## Debug controls

Not a developer build -- these are in the release, because the person looking at
something drawn in the wrong place is running the game they downloaded.

| Key | Does |
|---|---|
| **F1** | Opens and closes the debug menu |
| F2 | Nothing. RT64 binds it to a session-long ray-tracing toggle with nothing on screen to explain it; this port removes the binding. |
| F3 | Views RDRAM |
| F4 | Toggles texture replacement -- the quick way to see what a pack is changing |

F1 brings up two windows. **Game editor** is RT64's own: pause the game and keep
the frame interactive, right-click a pixel to see which draw calls made it,
browse the framebuffers and textures. **Wave Race HUD** is this port's: every 2D
element of the frame with its identity, its position in the game's own 320x240
pixels, and the class the widescreen rewriter gave it. Hover a row and that
element is outlined on screen; click to pin it; change the class from the
dropdown and it applies on the next frame.

If something sits in the wrong place, that window is how to say which thing.
[docs/HUD-INSPECTOR.md](docs/HUD-INSPECTOR.md) is the manual.

## Known issues

- **Windows** is what this project builds and tests today. A fork carries native
  macOS support; wider platform coverage is intended.

Not bugs, though they get reported as such: in a **4:3 window** the picture is
letterboxed, because the game draws a 303x199 region of its 320x240 framebuffer
and always did -- those are the overscan borders a CRT hid. Fullscreen on a
widescreen display, the default, has no bars.

## Layout

```
src/        the port: platform layer, renderer binding, input, audio, launcher wiring
include/    its headers
patches/    replacements and wrappers for individual game functions
recomp/     N64Recomp configuration
tools/      the build pipeline's scripts, and the diagnostics (see tools/README.md)
assets/     the launcher's stylesheet, icons and fonts
examples/   a mod template
docs/       the build guide, the technical reference, the plan and its findings
lib/        upstream submodules
```

Everything derived from a dump -- the disassembly, `RecompiledFuncs/`, the ELF --
is generated locally and refused by `.gitignore`. So is the splat config, which
is derived from the reference decompilation's.

## Documentation

[docs/README.md](docs/README.md) is the index. The two documents worth knowing
about if you are working on the Wave Race 64 decompilation or on another N64 port
are [docs/GAME-INTERNALS.md](docs/GAME-INTERNALS.md), which is about the game --
addresses, layouts, formats, drawing conventions, the audio microcode -- and
[docs/PORTING.md](docs/PORTING.md), which is about the toolchain and runtime,
written symptom-first. Both are kept current as the port changes.

## Contributing

[CONTRIBUTING.md](CONTRIBUTING.md) has the rules and the working conventions. The
short version: no copyrighted game material enters this repository -- no dump,
nothing extracted from one, and nothing derived from what was extracted, upscaled
textures included. Facts about the ROM (addresses, layouts, formats) are what the
documentation is made of and are welcome; the ROM's contents are not.

## Licensing

The project's own code and artwork are under the MIT License ([LICENSE](LICENSE)).

A built executable is another matter. It links N64ModernRuntime statically, and
N64ModernRuntime is **GPL-3.0**, so the executable as a whole is a GPL-3.0
combined work: anyone who distributes one must provide its complete source under
the GPL's terms, and this repository at the tagged commit, with the submodules it
pins, is that source. RecompFrontend, which supplies the menus, has published
**no license** at the time of this release. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the full list.

## How this was made

The entire project -- every line of code, every script, every document including
this one -- was written by Claude, Anthropic's AI model, working in
[Claude Code](https://claude.com/claude-code) under the direction of Daniel Gomes
Vieira, who set the goals, made the decisions, played the builds and reported
what he saw. The launcher artwork and the executable's icon were generated with
Claude as well. The work was done in phases (see [docs/PLAN.md](docs/PLAN.md)),
and each phase's findings are written up under
[docs/findings/](docs/findings), in the same way: by Claude, as the work was
done.

## Credits

### Upstream

The recompilation toolchain and runtime are by Mr-Wiseguy and the N64Recomp
contributors; RT64 is by Dario and the RT64 contributors. The function names this
project leans on come from the Wave Race 64 decompilation by LLONSIT and
contributors. Earlier recompilation attempts by WACOMalt and chronic8000 showed
what to expect.

### Elliott Tate

Three substantial parts of this port come from
[Elliott Tate](https://github.com/elliotttate), offered to this project in
[pull request #2](https://github.com/danielgomesvieira2000/wave-race-64-recomp/pull/2)
and built and played by him on an Apple M3 Max.

- **The modern water renderer.** All of it: the RT64 renderer, the interpolation
  that survives the game recentering its water lattice, the depth handling, the
  wake and foam fields, the shoreline intersection, the spray, the ten course
  profiles and the shaders. This repository moved it onto its own display-list
  rewriter, delivered it as an idempotent patch script, fixed two faults that
  only appear outside macOS, and wrote it up -- but the renderer is his.
  See [docs/WATER-IMPLEMENTATION.md](docs/WATER-IMPLEMENTATION.md), which
  separates what he built from what changed here.
- **Native macOS support.** The Metal window handle, the Apple paths, the bundle
  layout, the dylib bundling and signing, and the dependency build. It is
  untested by this project's maintainers and rests on his validation.
- **Controller rumble**, and the addresses this port reads a race from: the
  craft's speed, its vertical velocity, whether it is airborne, how wet the hull
  is, collisions, laps, buoys, misses, power, the countdown. They drive a larger
  feedback system in his fork than here. They are re-verified against a scripted
  race and recorded in [docs/GAME-INTERNALS.md](docs/GAME-INTERNALS.md)
  section 7, so the next port does not have to find them again.
