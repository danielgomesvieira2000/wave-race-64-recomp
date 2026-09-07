# Wave Race 64 Recompiled for macOS

This is the Apple Silicon release from
[elliotttate/wave-race-64-recomp](https://github.com/elliotttate/wave-race-64-recomp),
based on upstream 0.4.0 with native Metal support and configurable modern water.

[Watch the water showcase](https://www.youtube.com/watch?v=ikUGbLmPbvA).

## Requirements

- An Apple Silicon Mac (M1 or newer). This download does not support Intel Macs.
- macOS 15 or later. The deployment target and bundled libraries are built for
  macOS 15; runtime testing was performed on an M3 Max running macOS 27.
- Your own **Wave Race 64 (USA) (Rev A / v1.1)** ROM dump. The original US release,
  other regions, and Shindou are incompatible. The required big-endian `.z64`
  SHA-1 is `508dfc2d4caa42b6f6de5263d0aed5e44ac7966a`.

The download includes the required libraries; Homebrew and Xcode are not needed
to play. No ROM, game assets, saves, or personal settings are included.

## Install and play

1. Download and extract the Apple Silicon ZIP from the fork's Releases page.
2. Drag `WaveRace64Recomp.app` to Applications and open it.
3. If macOS blocks the first launch because the developer is unverified, use
   **System Settings → Privacy & Security → Open Anyway** for this app after
   attempting to open it, if you trust the download. The app is ad-hoc signed
   and is not notarized. See [Apple's first-launch guidance](https://support.apple.com/en-us/102445).
4. Select your USA Rev A `.z64` dump in the launcher, then start the game.

Keyboard controls: arrows steer, **X** accelerates, **C** is B, **Enter** is
Start, and **Escape** opens settings. Gamepads and controls can be configured
in the settings menu.

## Water settings

In **Graphics → Water**, choose **Modern** or **High**, then Apply. High adds
screen-space reflections and fine spray; Original remains the default.

The **Water** tab provides Modern/Classic appearance, Soft/Normal/Strong surface
ripples, and Spray particles Off/On. Classic retains the original palette and
transparency. Turning spray off keeps surface foam, wakes, and original splashes.
**F9** compares with Original; **F10** cycles diagnostic views.

Settings and saves are stored in
`~/Library/Application Support/WaveRace64Recomp`. Replacing the app preserves
that folder.

## Notes and source

The original wave simulation, handling, collisions, and timing remain
authoritative. Rendering has been checked at multiple requested refresh rates,
but this is not a guarantee of locked 120 FPS on every Mac or course. Some
upstream HUD/menu layout issues remain, and complete all-course playthrough
coverage is still pending.

`BUILD.json` identifies the exact source revision, executable checksum, and
pinned submodules. Clone that revision recursively and follow `docs/MACOS.md`
to rebuild; runtime patches and procedural water profiles are included in the
repository. `macos-dependencies.json` records the bundled libraries and their
source hashes. The program contains statically recompiled game code and loads
game assets from your dump. See `LICENSE`, `THIRD_PARTY_NOTICES.md`, and
`licenses/` for the project's and dependencies' notices.
