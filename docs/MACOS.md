# Native macOS build

The Apple Silicon build uses SDL2, RT64's Metal renderer, the RecompFrontend
launcher/settings UI, and the recompiled RSP audio. It builds a double-clickable
application and bundles its non-system dynamic libraries, replacement soundtrack,
and reviewed HD texture pack. New profiles start with Custom music, HD textures,
and High water with Modern appearance enabled. Existing saved choices persist.

For a ready-to-run download, see [the macOS release guide](MACOS_RELEASE.md).

Prerequisites: Xcode with the Metal Toolchain component, Clang, CMake, Ninja,
Python 3.10+, SDL2 and FreeType. SDL2 and FreeType can be installed with
`brew install sdl2 freetype`; the build does not use a Windows compatibility layer.
If shader compilation reports a missing Metal Toolchain, install it with
`DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcodebuild -downloadComponent MetalToolchain`
(use `Xcode-beta.app` if appropriate).

From the repository root, prepare the local Python environment and MIPS binutils:

```sh
bash tools/setup_macos.sh
bash tools/build_macos.sh "/path/to/Wave Race 64 (USA) (Rev A).zip"
open build-macos/WaveRace64Recomp.app
```

The first build accepts a ZIP or a `.z64`, `.n64`, or `.v64` dump, converts byte
order as needed, and requires the exact USA Rev A/v1.1 SHA-1. It disassembles the
dump, assembles and checks the code against the ROM, then generates the game and
audio sources. These private outputs are ignored by Git. Subsequent source-only
builds use `bash tools/build_macos.sh` without an argument.

The resulting app is `build-macos/WaveRace64Recomp.app`. It is ad-hoc signed for
local use, not notarized. The game ROM is kept outside the bundle. Select it in
the launcher; the runtime remembers it in the user's application-support data.
Settings and saves are under `~/Library/Application Support/WaveRace64Recomp`.

Keyboard defaults: arrow keys steer, X accelerates (A), C is B, Enter is Start,
and Escape opens the settings menu. Controllers can be remapped in settings.

## Portable release packaging

The default deployment target is macOS 15 (`WR64_MACOS_TARGET` overrides it).
RT64's native code and Metal shaders both honor that target. Homebrew bottles
may require a newer OS; release builds use separately compiled dependencies:

```sh
bash tools/build_macos_dependencies.sh
WR64_BUILD_DIR=build-macos-release \
WR64_DEPENDENCY_PREFIX="$PWD/build-macos-deps/install" \
bash tools/build_macos.sh
python3 tools/package_release_macos.py --version 0.4.0-macos.1 \
  --dependency-prefix build-macos-deps/install \
  --dependency-manifest build-macos-deps/dependencies.json
```

The dependency script verifies pinned source checksums and builds SDL2,
FreeType, and libpng for the selected target. The packaging script preserves
the signed app, includes notices/source provenance, rejects ROM/save files,
and writes a ZIP plus SHA-256 checksum under `dist/`. It refuses to replace
existing release outputs. The app's minimum-version metadata is derived from
the executable and bundled libraries. Test the extracted ZIP before publishing.

The build applies the repository's existing runtime patches and
`tools/patch_macos.py`, which fixes a missing standard-library include in the
pinned hlsl++ scalar implementation. Submodule modifications are intentional
and reproducible through those scripts.
