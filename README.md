# Wave Race 64: Recompiled

A native PC port of Wave Race 64 for Windows, Linux and macOS, made by statically
recompiling the game with [N64Recomp](https://github.com/N64Recomp/N64Recomp).
Unofficial, and not affiliated with Nintendo.

**You need your own dump of Wave Race 64 (USA) (Rev A)**, also called v1.1
(sha1 `508dfc2d4caa42b6f6de5263d0aed5e44ac7966a`). No other version works, and
none is included. Download a build from [Releases](../../releases), run it, and
pick your dump in the launcher.

## Features

- Widescreen at your display's resolution, with the HUD laid out across the frame or kept at 4:3
- High frame rate: smooth motion at your display's refresh rate, with the game's own physics and timing
- Field of View setting, 45 to 110 degrees
- Draw Distance: Original or Extended, with every buoy in view drawn and the sea out to the horizon
- Modern water renderer, optional: lighting, depth colour, refraction, wakes, shoreline foam, reflections and spray
- Controller rumble, which the original never had
- Audio with main and music volume
- Launcher with settings and controller remapping; controllers are assigned as they are plugged in
- Mod support, including texture packs, from the Mods tab
- Debug menu on F1: RT64's game editor, a HUD inspector and a per-course water sun editor

## Building

Full instructions, including toolchain setup, are in [docs/BUILDING.md](docs/BUILDING.md).

**Linux and macOS**

```sh
git clone --recurse-submodules https://github.com/danielgomesvieira2000/wave-race-64-recomp
cd wave-race-64-recomp
bash tools/setup_linux.sh --install        # or tools/setup_macos.sh
bash tools/build_linux.sh "/path/to/Wave Race 64 (USA) (Rev A).z64"   # or tools/build_macos.sh
```

**Windows** (needs Git, CMake, Ninja, LLVM, Python and Visual Studio Build Tools, plus WSL Ubuntu with `cmake ninja-build build-essential binutils-mips-linux-gnu`)

```powershell
git clone --recurse-submodules https://github.com/danielgomesvieira2000/wave-race-64-recomp
cd wave-race-64-recomp
git clone https://github.com/LLONSIT/Wave-Race-64.git reference/wr64-decomp
foreach ($p in 'rt64','n64recomp','rsprecomp','librecomp','recompinput','runtime_shutdown') { python tools/patch_$p.py }
wsl -d Ubuntu -- bash tools/wsl_build_recompiler.sh
python tools/generate_game.py "Wave Race 64 (USA) (Rev A).z64"
cmake -B build-fe -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DWR64_WITH_RUNTIME=ON -DWR64_WITH_RECOMPILED=ON -DWR64_WITH_FRONTEND=ON
cmake --build build-fe --target WaveRace64Recomp
```

Then run `build-fe\WaveRace64Recomp.exe`.

## Credits

- **N64Recomp and N64ModernRuntime** by Mr-Wiseguy and contributors
- **RT64** by Dario and contributors
- **RecompFrontend** by the N64Recomp contributors
- **Wave Race 64 decompilation** by LLONSIT and contributors, for the function names and symbols
- **[Elliott Tate](https://github.com/elliotttate)** for the modern water renderer, native macOS support, and the race addresses behind rumble
- WACOMalt and chronic8000, for earlier recompilation attempts
- Written by Claude (Anthropic) in Claude Code, under the direction of Daniel Gomes Vieira

## Licensing

The project's own code is MIT ([LICENSE](LICENSE)). A built executable links
N64ModernRuntime, which is GPL-3.0, so the executable is a GPL-3.0 combined work
whose source is this repository at the release's tag. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for every component.
