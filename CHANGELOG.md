# Changelog

Every release, newest first. Each entry links to its full notes under
[`docs/releases/`](docs/releases), which carry the reasoning, the measurements
and what was deliberately left out.

Versions follow [semantic versioning](https://semver.org) loosely: while the
project is below 1.0, the minor number moves when something a player would
notice changes.

## [0.8.1](docs/releases/0.8.1.md) — Draw Distance marked experimental

- **Draw Distance is marked experimental.** Anything above Original makes the
  buoys flicker and slide as they come into view: the game is being shown more of
  them than it was built to draw, and what decides which ones get drawn has not
  been found yet. It happens at every setting above Original, so it is not a
  matter of distance. Original is unaffected -- at that setting nothing is
  written to the game's memory at all.
- The limit is now written **once per frame on the game's own thread**, where it
  is read, rather than from the graphics thread once per display list. That race
  was real and is fixed; it was not the cause of the flicker.
- `WR64_DRAW_DISTANCE_TRACE=1` records the limit every frame, so the next look at
  this starts from data.
- **Eleven more 2D elements tagged**, bringing the port's built-in table to
  forty-three entries. Two of them reverse earlier decisions.

## [0.8.0](docs/releases/0.8.0.md) — clean audio, and the wipe

- **Draw Distance** in the Graphics tab, up to 4x. There is no global draw
  distance in this game -- the far plane is already twenty times further out than
  anything drawn, and each kind of object is culled by its own code -- so this is
  one setting over the limits that have been found. Today it reaches the buoys,
  which the game drops **between 2500 and 5000 units depending on the course**
  while drawing the world to 16,192. At 4x on a 2500 course the count drawn goes
  from a handful to the whole course.
- `WR64_3D_TRACE_EVERY` spaces the 3D trace's frames over a run instead of taking
  them consecutively, which is what a question about distance needs.
- **A fifth 2D class, `spill`**, for an element that is in the right place at
  the right size and is simply being cut off at the old frame's edge: it lifts
  the game's 4:3 scissor for that element and changes nothing else. The sun over
  a race is the case it was written for -- the game draws it running off the left
  of its own screen, and on a widened frame it stopped dead at the 4:3 boundary.
  Its disc, glare and haze ship tagged, bringing the built-in table to
  thirty-three entries.
- **The transition wipe between the select screens covers the whole frame.** It
  had been boxed in the middle 4:3 since 0.3.0 and was the port's oldest known
  issue. RT64 already renders a pass that covers the frame across the widened
  frame; what stopped this one was the viewport origin the port sets on a menu's
  perspective pass to keep 3D objects inside the boxes a layout gives them --
  right for the watercraft on the rider-select screen, wrong for a curtain meant
  to cover the screen. The origin now comes off for that call and goes back
  after. Telling the two apart means looking inside the called display list the
  curtain is drawn from, because the viewport that identifies it is loaded in
  there and is invisible at the call.
- **The audio crackle is fixed.** SDL takes a whole device period from the queue
  every time and fills whatever is missing with silence rather than waiting, and
  the queue was running at less than half a period -- empty in every one of the
  forty-five two-second windows of a ninety-second run, which put a 0.7 ms hole
  in the sound twenty-six times a second. The port now holds 30 ms in hand and
  asks the device for 256 frames at a time instead of 1024. The samples
  themselves were never at fault: a dump of the same run is clean.
- **The port resamples its own audio now**, and the output device is opened at
  the rate the machine actually runs rather than at the rate the game asks for.
  SDL was doing the conversion and losing continuity at every block boundary --
  measured at 14.4 dB of signal to everything-else on an 8 kHz tone, against 91.0
  for the replacement, which measures the same whether it is fed 64 frames at a
  time or 4096. The pitch stops drifting with it, and so does the gap in the
  sound at every entry to and exit from a race, because the device no longer has
  to be reopened when the game changes rate.
- **Closing the game no longer crashes the process.** It had been dying with an
  access violation on every normal exit, after everything had reported itself
  shut down. RT64 chains itself into SDL's event filter and never takes itself
  back off, so SDL still held a pointer to the destroyed object and the messages
  `SDL_DestroyWindow` pumps made a virtual call through freed memory.
  `tools/patch_rt64_eventfilter.py` supplies the missing half.
- The crash report now carries the faulting thread, a stack, and the module
  behind every frame -- and, when the fault is a jump into freed memory that
  nothing can unwind through, the return addresses recovered by scanning the
  stack. That is what identified the above.
- `WR64_AUDIO_STATS` and `WR64_AUDIO_DUMP` measure the audio path: how much
  silence the output device had to be given because nothing was queued in
  time, and what the samples sound like before SDL ever sees them. Between
  them they say which side of `SDL_QueueAudio` a crackle is on.
- `docs/GAME-INTERNALS.md` gains the world's frustum, the buoy cull and where its
  limit lives, the course arrows (which turn out **not** to be distance-culled at
  all -- they are navigational markers chosen by where the player is), and the
  animated water: a 500-vertex patch reaching 922 units whose spacing is computed
  per frame rather than stored, with the three ways of looking for it that do not
  work.

## [0.7.1](docs/releases/0.7.1.md) — placement and a tidier known-issues list

- Three more 2D elements placed correctly in widescreen, found with the debug
  menu and tagged into the port's built-in table, which now holds twenty-eight
  entries.
- The **results screen's HUD layout** and the **MAX POWER banner's last letter**
  are fixed.
- **Every course has now been played** in both directions.
- The known-issues list lost the things that were never faults: the **4:3
  letterbox** is the game's own overscan, recorded as a note rather than an
  issue, and the **announcer volume** is a property of how the game mixes its
  audio rather than something outstanding.

## [0.7.0](docs/releases/0.7.0.md) — mods

- **Mod support.** The Mods tab installs, enables and reorders mods, and the
  launcher has an entry for it. The runtime accepts `.nrm` mods carrying
  recompiled code or a ROM patch; the port adds **texture packs**, read straight
  out of the mod file and switchable without restarting.
- `tools/pack_mod.py` builds a `.nrm` from a directory, and
  [`examples/mods/`](examples/mods) carries a working template and the workflow.
- **Field of View** in the Graphics tab, 45 to 110 degrees.
- The repository was reorganised: this changelog, a documentation index, and the
  phase findings and release notes moved under `docs/findings/` and
  `docs/releases/`.

## [0.6.0](docs/releases/0.6.0.md) — a debug menu, and four things it found

- **F1 opens a debug menu in every build**, listing the frame's 2D elements with
  the class the widescreen rewriter gave each one, outlining one on the screen
  when you hover it, and letting you change that class while the game runs.
- Four widescreen faults found with it and fixed: the opening's **sun glare**
  now reaches the edges; the **menu cursor** stays beside the entry it marks; and
  the **craft on the results screen**, the **course overview's preview** and the
  **rider-select craft** line up with the layouts they belong to.
- **Quit** on the launcher menu.
- [`docs/HUD-INSPECTOR.md`](docs/HUD-INSPECTOR.md) is the manual, and its second
  half is a recipe for putting the same tool in another N64 port.

## [0.5.0](docs/releases/0.5.0.md) — the controller release

- **Rumble**, in a game that shipped a year before the Rumble Pak and has no
  rumble code at all: the feedback is worked out from the race itself. **Rumble
  Strength** in the General tab sets how hard, and zero turns it off.
- **Main Volume** applied for the first time, and **Music Volume** beside it,
  scaling the music inside the game's own audio engine.
- **Mute When Not In Focus.**
- **Pads are assigned as they are plugged in**, and **the keyboard works at the
  same time as a pad**. Remapping in the controls tab takes effect at last.
- **Steering is analogue again** — every deflection past the deadzone used to
  clamp to full lock.

## [0.4.0](docs/releases/0.4.0.md) — the frames in between

- The sky and the water move with everything else at high frame rates instead of
  stepping at the game's own.
- The opening no longer switches between its proper width and a magnified
  picture.

## [0.3.0](docs/releases/0.3.0.md) — the widescreen HUD

- The race HUD laid out across the frame, the game's full-screen overlays
  reaching its edges, and the warm-up round widened like every other race.
- **HUD Placement** in the Graphics tab chooses between the two.

## [0.2.0](docs/releases/0.2.0.md) — widescreen and split screen

- Widescreen fixes for the screens 0.1 got wrong, and two-player split screen
  presented in full.

## [0.1.0](docs/releases/0.1.0.md) — first release

- A native Windows port of Wave Race 64 (USA) (Rev A), statically recompiled:
  the game boots, its menus work, and championship, time trial and two-player
  races run with audio, at speed, with records saved to the emulated EEPROM.
