# Replacement music

Choose **Settings → Sound → Music → Custom** to play installed recordings.
**Original** restores cartridge music, including during play. **Custom music
volume** balances recordings against effects; **Main Volume** controls the final
mix. This fork includes nine replacement recordings in `assets/music`, and the
build copies them into the application. See the [recording credits](../assets/music/CREDITS.md)
and [source/checksum manifest](../assets/music/manifest.json). The music is separate
from this repository's MIT-licensed code.

To override an included recording, install a stereo **48 kHz PCM16 WAV** file in the `music` directory under the
game's settings directory. On macOS this is
`~/Library/Application Support/WaveRace64Recomp/music/`.
Restart the application after adding or changing files. Each local recording
takes priority over the bundled version, using the loop settings from its own
folder. Missing local tracks use the included recordings. An unreadable,
oversized, or incorrectly formatted local recording retains that cue's original
cartridge music. A partial local pack works.

| File | In-game placement | USA Rev A sequence |
| --- | --- | --- |
| `main_theme.wav` | Title / Main Theme | 3 |
| `options.wav` | Main menu / Options | 4 |
| `dolphin_park.wav` | Dolphin Park | 6 |
| `sunny_beach.wav` | Sunny Beach | 7 |
| `sunset_bay.wav` | Sunset Bay | 8 |
| `seafoam_shoreline.wav` | Marine Fortress, Drake Lake, Port Blue, Southern Island | 9, 10, 11, 14 |
| `twilight_city.wav` | Twilight City | 12 |
| `glacier_coast.wav` | Glacier Coast | 13 |
| `victory.wav` | First-place championship results | 15 |

The bundled Bryan EL / Retro Game Remix setup uses Bryan EL for Main Theme and
Dolphin Park. The other recordings are from Retro Game Remix's *Dolphin Park*.
The artist describes **Victory!** and **Seafoam Shoreline** as original
compositions, so their placements above are custom soundtrack choices rather
than claims of matching remixes. The duplicate Dolphin Park and Wave Race album
tracks do not override the Bryan EL recordings.
Artist/album: https://retrogameremix.bandcamp.com/album/dolphin-park

Convert an audio file with FFmpeg, mapping only its audio stream (MP3 artwork
must not be imported). For example:

```sh
ffmpeg -i input.mp3 -map 0:a:0 -vn -map_metadata -1 \
  -af volume=-3dB -ac 2 -ar 48000 -c:a pcm_s16le sunny_beach.wav
```

Use enough headroom for the source's decoded peak. Conversion does not itself
normalize loudness or identify musically seamless loops.

An optional `loops.json` sets repeat points and track-specific attenuation:

```json
{
  "main_theme": {
    "start_seconds": 75.0,
    "end_seconds": 171.634,
    "crossfade_seconds": 0.25,
    "gain_db": -3.5
  }
}
```

Playback starts at the beginning of the recording. At `end_seconds`, it repeats
to `start_seconds`, blending the final `crossfade_seconds` with the beginning
of the repeat; the next position is `start_seconds + crossfade_seconds`.
Without loop settings, the full recording repeats with a one-second crossfade
(shorter for very short recordings). These fades avoid sample discontinuities;
they do not guarantee a beat-matched musical edit. Invalid loop settings fall
back to the full recording. `gain_db` permits attenuation from -30 to 0 dB.

The native sequence scripts continue running because the title script also
drives game events. Only ready replacement cues have their native music gain
suppressed. Player zero, which owns effects and announcer commands, is excluded.
Original cue loads, fades, pause behavior, natural endings, and resets control
replacement playback. Mixing follows queued audio samples and the actual audio
sample rate, independently of rendering framerate.

For QA, `WR64_MUSIC_DIRECTORY` exclusively selects an alternate pack without
bundled fallback or changing saved settings. Pointing it at an empty directory
tests original-music fallback. `WR64_AUDIO_CAPTURE=/absolute/path/capture` writes numbered four-channel
WAV segments: native left/right followed by the final mixed left/right. WAV
headers finish on orderly process exit; capture is disabled by default.

Standalone mixer checks (requires SDL2) are in `tools/tests/music_test.cpp`.
Run its `missing`, `invalid`, `mix`, `bundled`, `local-override`, and `invalid-local`
scenarios in separate processes after
compiling it with `src/music.cpp`, the project include directory, the runtime's
`thirdparty` include directory, and SDL2. They exercise real samples, resampling,
loop boundaries, pause/resume, restart, stop, fallback, volume, saturation,
bundled playback, and local overrides.
