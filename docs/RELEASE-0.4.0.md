# Wave Race 64: Recompiled 0.4.0

The frames in between: the sky and the water move with everything else at high
frame rates instead of stepping at the game's own, and the opening no longer
switches between its proper width and a magnified picture.

**You need your own dump of Wave Race 64 (USA) (Rev A), also called v1.1**
(revision 1, header CRC `0x492F4B61 0x04E5146A`). No other version works. The
launcher checks the file you pick.

## Download

`WaveRace64Recomp-0.4.0-windows-x64.zip`: unzip anywhere, run
`WaveRace64Recomp.exe`, pick your dump. Settings and saves go to
`%LOCALAPPDATA%\WaveRace64Recomp`, and so does `wr64.log`, the file to attach
to a bug report. Settings from earlier versions carry over.
`WaveRace64Recomp-0.4.0-windows-x64-debug-symbols.zip` is not a second build:
it holds the debug symbols for the same executable, only needed for a readable
crash report.

The zip contains the recompiled game code and none of the game's assets; the
executable is a GPL-3.0 combined work (see the README's *Licensing*), and its
source is this repository at tag `v0.4.0`.

## What changed

Everything here is about the Framerate setting. The game keeps running at the
rates it chose for itself -- 20 in the menus, 30 in a race -- and the renderer
draws the frames in between by working out how each thing moved since the last
one. Two of the biggest things on screen were not moving as far as it could
tell, so they alone stayed at the game's rate.

- **The sky moves with the rest of the picture.** It used to shimmer along the
  horizon at 60 frames a second, worse above it. The game rebuilds the sky's
  three bands from scratch every frame rather than moving them, so the renderer
  saw a thing that never moved and held it still between the game's frames
  while the world glided past it.
- **The water's waves are smooth.** The same thing, in the place it matters
  most: the surface is rebuilt every frame, so the waves stepped at 20 or 30
  frames a second under a camera running at 60.
- **The opening keeps one width.** It switched between its proper widescreen
  and a magnified, stretched picture from one camera shot to the next -- most
  visible on the Wave Race logo in the corner, which grew and slid towards the
  edge. One rectangle covering the whole frame was being treated as a
  full-screen overlay to stretch, when it belongs to the picture rather than
  lying over it.

If either of the first two looks worse to you than it did, they can be switched
off one at a time: set `WR64_NO_SKY_INTERP=1` or `WR64_NO_WATER_INTERP=1` in the
environment before starting. `WR64_PAIRING=1` writes what the renderer is
managing to interpolate into `wr64.log`, which is the useful thing to attach if
something judders.

## Known issues

- In the **main menu** and the **name-entry screens**, a few elements alternate
  between centred and stretched every few frames. It is the same kind of flicker
  the opening had, from a different cause, and it is not fixed here.
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
Run `python tools/patch_rt64.py` before configuring: it carries the patches
this port needs, and a submodule update reverts them silently. It gained one
since 0.3.0, which is what lets the port report how well the frames in between
are being built.
