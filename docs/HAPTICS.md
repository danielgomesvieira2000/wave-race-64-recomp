# Controller haptics

Open **Settings → Haptics**. The default is **Events only**, with feedback strength at
80%. Choose **Full** to add continuous engine/water ambience (35% by default),
or **Off** to disable everything.
Setting feedback strength to zero also stops all output. Apply saves your choice.

| Situation | Feedback |
|---|---|
| Accelerating and skimming water | Restrained engine vibration and changing water texture, scaled by actual speed, throttle, wet contact, and steering |
| Leaving the water | Short, light release; the continuous water/engine bed fades while airborne |
| Landing | A weighted thump with a softer tail, scaled by airborne duration and native downward velocity |
| Small wave contacts | Short, lighter taps with a cooldown |
| Craft or barrier collision | A sharp two-motor impact scaled by the game's collision force |
| Crash and recovery | One heavy impact followed by a fading bump; propulsion stays quiet throughout recovery |
| Correct buoy | A crisp tick, including passes at maximum power |
| Missed buoy | Two lower, heavier pulses |
| Power gain | A rising pair of pulses |
| Countdown and GO | Short countdown beats, then a stronger launch cue |
| Completed lap / finish | Distinct ascending sequences; entering lap one does not count as completing a lap |
| Retirement | A descending two-pulse cue |

Normal controller rumble works through SDL's two motor channels. Trigger
vibration is added only when the device and driver advertise support; disabling
it leaves normal rumble available. This is trigger vibration, not adaptive
trigger resistance or an audio-driven DualSense waveform. Controllers without
rumble continue to work normally. Output follows the primary human controller
used by this port's current input path; it does not vibrate for AI riders.

Feedback stops in the native pause menu, frontend settings, and when the window
loses focus. A short output duration prevents a sustained effect if the main
loop stalls. Disconnecting, changing controllers, or quitting clears output.

## Integration and verification

The guest game thread captures an immutable observation at graphics-task
submission, after the original physics and race update. Observations are read
only and use the human race-slot mapping, not the selected character's identity.
Event detection deduplicates native ticks; envelopes use monotonic elapsed time
on the main thread. SDL refreshes at up to 125 Hz with a 50 ms expiry. It does
not depend on whether the water renderer is Original, Modern, or High, and it
does not advance game simulation or modify input, physics, race statistics,
audio, or rendering state.

USA Rev A sources are documented in `src/haptics_observation.cpp`: native wet
contacts and vertical velocity, craft/barrier closing-speed maxima, the full
crash/recovery interval, checkpoint/outcome and total-miss counters, actual
power, lap completion, and the original countdown. Terminal observations remain
available during the postrace camera so finishing cannot discard its cue.

Run the focused checks with:

```sh
.venv/bin/python -m unittest tools.tests.test_haptics_mixer tools.tests.test_haptics_output tools.tests.test_default_options -v
```

The mixer tests use synthetic observations and an 8 MiB word-swapped memory
fixture. Output tests use SDL virtual-controller callbacks to verify independent
channels, refresh and expiry, mute, reconnect/reroute, unsupported capabilities,
failure handling, and invalid values without vibrating physical controllers.

For native evidence, set `WR64_HAPTICS_TRACE=/absolute/path/feedback.csv`. The
game writes native observations and event names there, and elapsed-time motor
levels to `feedback.csv.output.csv`. The parent directory must exist. Set
`WR64_HAPTICS=off` for a process-only comparison; saved settings remain unchanged.
These traces are local diagnostics and are not bundled. Controller availability
and the first accepted output are logged; accepted driver commands do not prove
subjective feel, which should be tuned during a hands-on controller playtest.
