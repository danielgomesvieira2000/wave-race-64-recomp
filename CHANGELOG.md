# Changelog

Every release, newest first. Each entry links to its full notes under
[`docs/releases/`](docs/releases), which carry the reasoning, the measurements
and what was deliberately left out.

Versions follow [semantic versioning](https://semver.org) loosely: while the
project is below 1.0, the minor number moves when something a player would
notice changes.

## Unreleased

- **Measured, on both platforms.** The sea extension and the raised draw
  distance cost nothing measurable: across five configurations of the same
  attract demo, 84 two-second windows each, the port held the game's own 20
  frames per second in 98% of windows with and without them, and 96% with all
  of it on at once — High water, the ring, and Maximum distance. The Linux
  build was rebuilt against all of it and runs on Mesa's software Vulkan.

- **The sea reaches as far as the course does.** The game animates a patch of
  water 922 units across and paints everything beyond it onto the sky — a flat,
  seven-vertex band at sea level, which is why the water appeared to meet the
  horizon while stopping almost at the player's feet. The port now draws the
  missing surface: a ring of quads from the patch's edge out to whatever Draw
  Distance asks for, its heights taken from the same wave field the game's own
  waves stand on, so the swell outside agrees with the swell inside. It carries
  the game's own texture coordinates and translucency, so it works with the water
  renderer **on or at Original**, and it appears as soon as either Water quality
  or Draw Distance is raised. `WR64_NO_WATER_RING=1` switches it off.

- **Draw Distance rebuilt, and no longer experimental.** It was a multiplier over
  a list of per-object limits that was expected to grow. A census of every
  display-list call over 5,000 race frames on two courses, repeated with one
  field doubled, found there is no list: a single integer per course decides how
  much of the course is submitted, and **49 of the 57 static kinds measured moved
  out with it** — buoys, gate markers, the shoreline and the props alike. So the
  setting is now one number, and it is a **distance** rather than a multiplier,
  because the game's own value is between 2,500 and 6,000 depending on the course
  and a multiplier therefore meant something different on every one. The choices
  are Original, 8,000, 12,000 and 16,192 — the far plane, past which nothing can
  be drawn at all; the furthest object measured on any course sat at 13,175.
  Original still writes nothing to the game's memory.
- **A draw-distance bug that silently disabled the setting on some courses.** The
  old code decided the game had reloaded the struct by comparing the field
  against what it had written. With the values the game actually uses that
  collides — 2,500 doubled is 5,000, which is another course's own limit — so
  moving between those two courses left the port believing its work was done and
  the second course ran at the game's own distance. It now keys on the course
  number, and waits for the course to load before reading the value it is going
  to scale.
- **The "flicker and slide" warning is gone, because it was about the wrong half
  of the pipeline.** At twice the limit the buoys are drawn at exactly the same
  39 world positions, and a position inside the limit blinks off for a single
  frame in 0.01% of its chances, against 0.03% at the game's own distance. The
  game submits correct, stable geometry at any of these settings. What slides is
  RT64 pairing one buoy's matrix with another's between frames as the set of them
  changes — confirmed by presenting at the game's own frame rate, where no frames
  are interpolated and the sliding does not happen. It is there at the game's own
  distance too, and it is written up in `docs/PORTING.md` under *Objects that
  slide when more of them are drawn*.

## [0.9.0](docs/releases/0.9.0.md) — Linux, macOS, and modern water

- **Linux and macOS builds.** The port builds and runs from the same tree on all
  three platforms. `tools/setup_linux.sh --install` then `tools/build_linux.sh`
  takes a Linux machine from a clean clone to a running game; the macOS pair does
  the same on Apple Silicon, producing a double-clickable, ad-hoc signed `.app`.
  Verified on Ubuntu 26.04 with Clang 21. **macOS is untested by this project** —
  it is [Elliott Tate](https://github.com/elliotttate)'s work, built and played
  by him on an M3 Max.
- **A modern water renderer**, in its own **Water** tab beside Graphics,
  defaulting to **High with the Aqua style**. It keeps the cartridge's waves,
  physics and timing exactly as they are and changes only how that surface is
  shaded: sun and sky lighting, colour that deepens with the water, refraction,
  persistent wakes, shoreline wash, and on High reflections of the scenery in
  view and fine airborne spray. **Water style** runs Classic, Modern, Aqua;
  brightness, tint and clarity shape Aqua; ripples and spray are separate.
  **Original** is a true bypass. `F9` compares the two from the same camera,
  `F10` cycles the diagnostics.
  Also Elliott Tate's, from
  [pull request #2](https://github.com/danielgomesvieira2000/wave-race-64-recomp/pull/2)
  — verified here on Windows and D3D12, which that branch had not been.
  Expect the time spent drawing a frame to roughly double above Original; see
  [docs/WATER.md](docs/WATER.md).
- Release packaging for the new platforms: `tools/package_release.py` writes a
  `.tar.gz` on Linux and a `.zip` of the signed bundle on macOS, beside the
  Windows `package_release.ps1`. `tools/build_macos_dependencies.sh` builds
  SDL2, FreeType and libpng from checksum-pinned source against the bundle's
  deployment target, so a published `.app` does not demand a newer macOS than it
  claims to.
- Settings and saves follow each platform's convention:
  `~/Library/Application Support/WaveRace64Recomp` on macOS,
  `$XDG_DATA_HOME/WaveRace64Recomp` on Linux.
- An intermittent crash on exit is fixed: the runtime was releasing its thread
  queues and RDRAM without waiting for the workers still reading them.
- **`--version` prints the actual version.** It had said 0.4.0 since 0.4.0; the
  version now comes from the single place it is declared.
- Three more 2D elements placed correctly, bringing the built-in table to 41
  entries.
- The `tools/` scripts no longer assume a checkout path or a WSL host. They
  derive the repository root from their own location, find MIPS binutils
  natively where it exists, and accept a versioned `clang-21` where Debian and
  Ubuntu ship no unversioned one.
- Two new reference documents: [docs/WATER.md](docs/WATER.md), and
  [docs/WATER-IMPLEMENTATION.md](docs/WATER-IMPLEMENTATION.md), which describes
  the fork's implementation in full and states exactly what differs here.

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
