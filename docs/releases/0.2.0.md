# Wave Race 64: Recompiled 0.2.0

Widescreen fixes for the screens 0.1 got wrong, and two-player split screen
presented in full.

**You need your own dump of Wave Race 64 (USA) (Rev A), also called v1.1**
(revision 1, header CRC `0x492F4B61 0x04E5146A`). No other version works. The
launcher checks the file you pick.

## Download

`WaveRace64Recomp-0.2.0-windows-x64.zip`: unzip anywhere, run
`WaveRace64Recomp.exe`, pick your dump. Settings and saves go to
`%LOCALAPPDATA%\WaveRace64Recomp`, and so does `wr64.log`, the file to attach
to a bug report. Settings from 0.1 carry over.
`WaveRace64Recomp-0.2.0-windows-x64-debug-symbols.zip` is not a second build:
it holds the debug symbols for the same executable, only needed for a readable
crash report.

The zip contains the recompiled game code and none of the game's assets; the
executable is a GPL-3.0 combined work (see the README's *Licensing*), and its
source is this repository at tag `v0.2.0`.

## What changed

- **Watercraft and rider select in widescreen.** The 3D models sit in their
  2D frames again. 0.1 drew them in the widened frustum while the frames
  stayed at 4:3, so they landed outside. Menus with a 3D backdrop, such as the
  title and the course overview, keep the backdrop at full width.
- **Two-player split screen in full.** 0.1 cropped the presented picture to
  the single-player drawn region, and the split screen, which is taller, lost
  its top and bottom. The crop now follows the region the game actually draws.
- Both come from a display-list rewriter in the port, which inserts RT64's
  extended commands into the game's display lists before RT64 sees them; the
  game's code and data are untouched. `docs/PLAN.md`, phase 07, describes it.

## Known issues

- Full-screen 2D effects the game draws, such as the sun's glare, stay at 4:3
  in the middle of the widened frame with the HUD and do not reach the sides.
- *HUD Placement* in the Graphics tab has no effect on this game yet.
- A 4:3 window shows the picture letterboxed; fullscreen on a widescreen
  display does not.
- Stunt mode and the championship ceremony have had less testing than the
  races; not every course has been played in both directions.
- Windows only.

## Building from source

See `docs/BUILDING.md`. The build needs your dump and a checkout of the
reference decompilation; nothing derived from either is in the repository.
