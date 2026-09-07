# Modern water performance on Apple M3 Max

Measured on Apple M3 Max, 48 GiB unified memory, macOS 27.0, using Metal. All fifteen resolution/rate replays exited with status 0. The bundle and profiles stayed unchanged across that matrix. A subsequent correction anchors field resets to the game interval boundary; the final build was rechecked at 1080p below. Shader sources, quality budgets and render-target layouts are unchanged by that correction.

The GPU timer covers the game rendering workload, including water shading, scene copies, depth reduction, normals, interaction fields and spray. The separate VI presentation/UI queue and display latency are outside this timer. CPU preparation measures renderer command preparation and uploads; it excludes GPU waits and is not a measurement of the original game simulation.

The one-player runs use Sunny Beach Time Trial; the two-player runs use the original versus mode. All use 4× MSAA. Each Modern/High run matched its corresponding Original run at all 49 game checkpoints. Measurements use moving submissions 1190–1390, before the pause, with no field readbacks, pass queries, debug shading or recording.

## Final build spot check

The corrected bundle passed another Original/Modern/High 1080p/60 Hz group,
603 moving frames per tier, the same 1920×986 target and 4× MSAA. Each
replacement again matched all 49 gameplay checkpoints. The final incremental
medians are **0.640 ms for Modern** and **1.109 ms for High**.

| Quality | GPU median | GPU p95 | CPU preparation median | Water payload |
| --- | ---: | ---: | ---: | ---: |
| Original | 0.970 ms | 1.420 ms | 0.201 ms | 0 MiB |
| Modern | 1.610 ms | 2.708 ms | 0.475 ms | 62.1 MiB |
| High | 2.078 ms | 3.089 ms | 0.492 ms | 74.1 MiB |

The first replacement frame measured 5.575 ms CPU / 3.612 ms GPU for Modern
and 6.135 ms CPU / 6.172 ms GPU for High, with the same warm-cache limitation.
The full results and executable/profile hashes are under
`build/water-qa/reset-performance/`. The temporal acceptance report in
`build/water-qa/reset-boundary/result.json` identifies the same final executable.

## Resolution and rate matrix

| Requested output / rate | Original median | Modern median | High median | High p95 | High delta vs Original |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1080p / 60 Hz | 0.963 ms | 1.576 ms | 2.017 ms | 2.993 ms | 1.054 ms |
| 1440p / 60 Hz | 1.467 ms | 2.089 ms | 2.440 ms | 2.959 ms | 0.973 ms |
| 2160p / 60 Hz | 1.689 ms | 2.934 ms | 4.616 ms | 5.056 ms | 2.927 ms |
| 1080p / 60 Hz / split screen | 0.917 ms | 1.906 ms | 2.632 ms | 3.416 ms | 1.716 ms |
| 1080p / 120 Hz | 1.134 ms | 1.326 ms | 1.695 ms | 2.924 ms | 0.562 ms |

At 1080p/60 Hz, Modern adds 0.613 ms and High adds 1.054 ms to the median game-rendering GPU cost, below the proposed incremental targets of 2 ms and 4 ms. These are observations for this scene and machine, not guarantees for every course or GPU. Dynamic GPU clocks and other system work were not locked.

The original game renders a cropped active region. The measured one-player targets are 1920×986, 2560×1314 and 3840×1971 within the requested outputs; split screen uses 1920×1031. Original and replacement extents match in every comparison. Both replacement split-screen views are present in every measured frame.

## CPU preparation and water resource payload

| Configuration | Modern CPU median | High CPU median | Modern payload | High payload |
| --- | ---: | ---: | ---: | ---: |
| 1080p / 60 Hz | 0.461 ms | 0.479 ms | 62.0 MiB | 74.1 MiB |
| 1440p / 60 Hz | 0.481 ms | 0.499 ms | 106.9 MiB | 119.0 MiB |
| 2160p / 60 Hz | 0.501 ms | 0.531 ms | 235.3 MiB | 247.3 MiB |
| 1080p / 60 Hz / split screen | 0.640 ms | 0.682 ms | 119.8 MiB | 131.9 MiB |
| 1080p / 120 Hz | 0.464 ms | 0.485 ms | 62.0 MiB | 74.1 MiB |

Payload counts water textures and upload/default buffers. It excludes driver alignment, pipeline objects and the original renderer's attachments. Process RSS was sampled every half second and is included in the JSON; on unified-memory macOS it must not be treated as a complete GPU-memory measurement. Hardware bandwidth counters were not collected.

## First replacement frame

| 1080p / 60 Hz | CPU preparation | GPU rendering |
| --- | ---: | ---: |
| Modern | 6.299 ms | 3.101 ms |
| High | 6.464 ms | 5.211 ms |

These include first-frame resource/pipeline work in each fresh process. The system shader cache was not purged, so they are not cold-driver-cache measurements.

## Reproduction

```sh
python3 tools/run_water_matrix.py --suite performance --output build/water-qa/NEW_PERFORMANCE_RUN
```

The complete medians, p95/p99 values, first-frame samples, render extents, view counts, CPU preparation and sampled RSS are in `build/water-qa/final-performance/performance.json`. Each run retains its frame CSV, state/input traces, settings, executable/profile hashes and clean-shutdown result. `machine.json` and `bundle.json` identify the tested environment.
