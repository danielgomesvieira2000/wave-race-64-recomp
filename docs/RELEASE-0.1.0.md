# Wave Race 64: Recompiled 0.1.0

The first public release. A native Windows port of Wave Race 64 (USA) (Rev A),
statically recompiled.

**You need your own dump of Wave Race 64 (USA) (Rev A), also called v1.1**
(revision 1, header CRC `0x492F4B61 0x04E5146A`). No other version works. The
launcher checks the file you pick.

## Download

`WaveRace64Recomp-0.1.0-windows-x64.zip`: unzip anywhere, run
`WaveRace64Recomp.exe`, pick your dump. Settings and saves go to
`%LOCALAPPDATA%\WaveRace64Recomp`, and so does `wr64.log`, the file to attach
to a bug report. `WaveRace64Recomp-0.1.0-windows-x64-symbols.zip` holds the
debug symbols; you only need it if asked for a readable crash report.

The zip contains the recompiled game code and none of the game's assets; the
executable is a GPL-3.0 combined work (see the README's *Licensing*), and its
source is this repository at tag `v0.1.0`.

## What works

- Title, menus, championship and time trial, with audio, records saved.
- Widescreen at the display's resolution, HUD at its original shape, the
  game's own black overscan borders removed.
- High frame rate: the game keeps its own 30 Hz (20 in menus and Time Trial)
  and RT64 interpolates each object's movement between game frames up to the
  display's refresh rate. Graphics tab, *Framerate*.
- Launcher, settings menu, controller remapping with per-device profiles.

## Known issues

- A 4:3 window shows the picture letterboxed; fullscreen on a widescreen
  display does not.
- *HUD Placement* in the Graphics tab has no effect on this game.
- Stunt mode and the championship ceremony have had less testing than the
  races; not every course has been played in both directions.
- Windows only.

## Building from source

See `docs/BUILDING.md`. The build needs your dump and a checkout of the
reference decompilation; nothing derived from either is in the repository.
