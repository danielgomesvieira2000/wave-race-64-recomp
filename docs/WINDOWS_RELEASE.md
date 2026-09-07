# Windows release

Download **WaveRace64Recomp-0.4.0-windows-x64.zip** from
[the release page](https://github.com/elliotttate/wave-race-64-recomp/releases/latest).
Extract the entire ZIP to a writable folder, then run `WaveRace64Recomp.exe`.
Keep the three DLLs and `assets` folder beside the executable. Build tools and
WSL are not required to play. The separate debug-symbols ZIP is optional and
only needed when investigating a crash.

This is a native Windows x64 build with Direct3D 12 rendering. Supply your own
**Wave Race 64 (USA) (Rev A / v1.1)** `.z64` in the launcher. Required SHA-1:
`508dfc2d4caa42b6f6de5263d0aed5e44ac7966a`. Other revisions and regions are
incompatible. The package contains no ROM, original asset dumps, saves, or
personal settings.

## Included enhancements

- **HD textures:** 1,828 replacement mappings across 1,806 images, with manifests
  and credits. The reviewed pack preserves the original designs; some mappings
  retain source-preserving artwork rather than adding new detail.
- **Replacement music:** nine recordings covering all courses, the main theme,
  options, and first-place results. Bryan EL's Main Theme and Dolphin Park
  remakes are joined by seven recordings from Retro Game Remix's *Dolphin Park*
  album. Credits and loop metadata are included. Effects and announcer audio
  retain the original sound.
- **Modern water:** reflections, refraction, shoreline wash, wakes, and spray.
  The original wave mesh and game physics remain authoritative.

Fresh installs select **Graphics → Textures → HD**, **Graphics → Water → High**,
**Water → Appearance → Modern**, and **Sound → Music → Custom**. Choose Original
under Textures, Water quality, or Music to restore that feature's original
presentation. Saved preferences are preserved when upgrading;
select the enhanced options manually if you previously chose Original.
A user-installed texture pack takes precedence over the bundled pack.

## Controls and saved data

Keyboard defaults: arrows steer, **X** accelerates, **C** is B, **Enter** is Start,
and **Escape** opens settings. Controllers and remapping are supported in the
launcher. **F9** compares water modes; **F10** cycles water diagnostics.

Settings, controller profiles, saves, and `wr64.log` live in
`%LOCALAPPDATA%\WaveRace64Recomp`.

To verify the download, compare the result below with the companion `.sha256`
file on the release page:

```powershell
Get-FileHash -Algorithm SHA256 .\WaveRace64Recomp-0.4.0-windows-x64.zip
```

## Validation and source

The Windows build was tested on an NVIDIA RTX 5090 using Direct3D 12 with
60 Hz presentation and 4x MSAA. A Sunny Beach Time Trial replay with the shipped
HD/Custom/High/Modern defaults completed 1,800 ticks and exited cleanly. All 49
game-state checkpoints through tick 1410 matched an Original-water replay.
Captured audio confirmed active game audio and replacement music mixing.

All 32 DXIL/SPIR-V water shader compilations passed. Nine bundled-asset
tests and four default-setting tests passed, along with patch reconstruction
and asset integrity checks. This is startup and race smoke coverage, not a
complete playthrough of every course and mode. Windows Vulkan playback and
other GPU families have not been directly tested here. Screen-space reflections
depend on visible scene geometry.

This build includes the Direct3D 12 water root-signature fix and corrected
CPU/RSP source generation. Windows release source is tagged
[`v0.4.0-windows.1`](https://github.com/elliotttate/wave-race-64-recomp/tree/v0.4.0-windows.1).
See [build instructions](https://github.com/elliotttate/wave-race-64-recomp/blob/v0.4.0-windows.1/docs/BUILDING.md),
[texture details](https://github.com/elliotttate/wave-race-64-recomp/blob/v0.4.0-windows.1/docs/HD_TEXTURES.md),
and [music mapping and credits](https://github.com/elliotttate/wave-race-64-recomp/blob/v0.4.0-windows.1/docs/MUSIC.md).
