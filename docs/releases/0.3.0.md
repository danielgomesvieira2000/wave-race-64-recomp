# Wave Race 64: Recompiled 0.3.0

The widescreen HUD: the race HUD laid out across the frame, the game's
full-screen overlays reaching its edges, and the warm-up round widened like
every other race.

**You need your own dump of Wave Race 64 (USA) (Rev A), also called v1.1**
(revision 1, header CRC `0x492F4B61 0x04E5146A`). No other version works. The
launcher checks the file you pick.

## Download

`WaveRace64Recomp-0.3.0-windows-x64.zip`: unzip anywhere, run
`WaveRace64Recomp.exe`, pick your dump. Settings and saves go to
`%LOCALAPPDATA%\WaveRace64Recomp`, and so does `wr64.log`, the file to attach
to a bug report. Settings from earlier versions carry over.
`WaveRace64Recomp-0.3.0-windows-x64-debug-symbols.zip` is not a second build:
it holds the debug symbols for the same executable, only needed for a readable
crash report.

The zip contains the recompiled game code and none of the game's assets; the
executable is a GPL-3.0 combined work (see the README's *Licensing*), and its
source is this repository at tag `v0.3.0`.

## What changed

- **The race HUD is laid out across the frame** when HUD Placement is set to
  anything but Original: time top-left, rank and lap top-centre, speed and the
  leaderboard right, MISS bottom-left, power bottom-right. Menus keep their
  4:3 layouts, which is what their frames are drawn for.
- **The game's full-screen overlays reach the edges.** The tint over a race and
  the dim the pause screen lays over the world both stopped short of the sides,
  leaving a brighter band down each edge.
- **The warm-up round is widescreen**, and truly so rather than a stretched
  4:3 picture. Every other race was already widened; this one was not, for a
  reason peculiar to how its frame is built.
- Which screens count as races is now read from the frame itself rather than
  from a list of the game's state numbers, so a race screen the list did not
  name is no longer mistaken for a menu and squeezed.

## Known issues

- The **results screen**'s layout is wrong when HUD Placement is set larger
  than Original.
- The **MAX POWER** banner loses its last letter while the HUD layout is on.
  It is drawn complete without it.
- A 4:3 window shows the picture letterboxed; fullscreen on a widescreen
  display does not.
- Stunt mode and the championship ceremony have had less testing than the
  races; not every course has been played in both directions.
- Windows only.

## Building from source

See `docs/BUILDING.md`. The build needs your dump and a checkout of the
reference decompilation; nothing derived from either is in the repository.
Run `python tools/patch_rt64.py` before configuring: it carries four patches
this port needs, and a submodule update reverts them silently.
