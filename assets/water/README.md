# Water profiles

`profiles.json` contains original, artist-authored parameter presets for the ten
course IDs in the USA Rev A game (including Rider Selection). No cartridge art
or third-party textures are included. Color triplets are linear RGB; absorption
is per game world unit. These are initial art directions pending the full course
comparison matrix, not measurements of the original lighting.

The application loads this versioned file from its packaged assets. A malformed
profile leaves the compiled default material available and prints a diagnostic.
`WR64_WATER_PROFILES` can select an explicit development file on launch.

Fine ripples, clouds, and field disturbances are generated procedurally in the
project's water shaders. Asset changes use the repository's project license.
