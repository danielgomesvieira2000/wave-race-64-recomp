# Building

## Requirements

| Tool | Version | Why |
|---|---|---|
| Git | any recent | submodules |
| CMake | 3.20+ | required by N64Recomp and the runtime |
| Ninja | any | the generator every upstream project assumes |
| Clang | 15+ | N64Recomp's output is validated against Clang and MSVC |
| Python | 3.10+ | splat and the symbol tooling in `tools/` |
| MIPS binutils | any | assembling the disassembly into an ELF (phase 01) |

**Do not build with modern GCC.** Recompiled output has a documented history of
miscompiling under modern GCC's optimizer. CMake will warn if you try.

### Windows

Verify what you have:

```powershell
pwsh -File tools/check_toolchain.ps1
```

Install what you don't (run in an elevated terminal):

```powershell
winget install --id Kitware.CMake
winget install --id Ninja-build.Ninja
winget install --id LLVM.LLVM
winget install --id Python.Python.3.12
winget install --id Microsoft.VisualStudio.2022.BuildTools `
    --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

The Build Tools install supplies the Windows SDK and the C runtime that Clang
links against on Windows; Clang alone is not enough.

A MIPS assembler is not available through winget. Phase 01 needs one of:

- **WSL** (`sudo apt install binutils-mips-linux-gnu`) — simplest, and what most
  decompilation projects assume.
- A prebuilt `mips64-elf` toolchain on `PATH`.

The Microsoft Store `python` stub on a fresh Windows install is not Python. If
`python --version` opens the Store, install the real thing with the winget
command above.

## Clone

```
git clone --recurse-submodules <this repo>
```

If you already cloned without submodules:

```
git submodule update --init --recursive
```

## Configure and build

The project is phase-gated so a partially finished tree always builds. Phase 00
needs no options:

```
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
cmake --build build
```

Later phases turn on what they earn:

| Option | Turned on by | Effect |
|---|---|---|
| `WR64_BUILD_RECOMPILER` | phase 02 | builds the N64Recomp tool from `lib/` |
| `WR64_WITH_RECOMPILED` | phase 02 | compiles the generated `RecompiledFuncs/` |
| `WR64_WITH_RUNTIME` | phase 03 | links ultramodern, librecomp and RT64 |

## Verify your dump

```
./build/WaveRace64Recomp --identify path/to/your.z64
```

Exit code 0 means the dump matches the pinned target. Anything else means stop
and fix that before continuing — a mismatched revision produces failures later
that look like tooling bugs.

## Known issues

### CMake 4.x and old vendored dependencies

CMake 4 removed compatibility with `cmake_minimum_required(VERSION <3.5)` and
hard-errors on it. Six files under `lib/RT64/src/contrib` still declare a lower
minimum:

```
imgui/examples/example_glfw_vulkan   2.8
implot/.github                       3.0
mupen64plus-win32-deps/SDL2*/cmake   3.0
re-spirv/external/SPIRV-Headers/...  3.0
```

All six are example, CI, or `find_package` config files that RT64's build does
not normally descend into, so this is expected to be harmless. If configure
fails naming one of them, the workaround is:

```
cmake -B build -G Ninja -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ...
```

Do not "fix" this by editing the submodules.

### Clang is not added to PATH by its installer

`winget install LLVM.LLVM` installs to `C:\Program Files\LLVM\bin` without
registering it. Add that directory to your user PATH.

## Building with the runtime (phase 03 onward)

Use **clang-cl**, not `clang++`, once `WR64_WITH_RUNTIME=ON`:

```
cmake -B build-rt -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl       -DWR64_WITH_RUNTIME=ON -DWR64_WITH_RECOMPILED=ON
cmake --build build-rt
```

RT64 decides its warning flags from `CMAKE_CXX_SIMULATE_ID`: a Clang targeting
the MSVC ABI gets `/W4`, on the assumption that such a Clang is `clang-cl`. That
assumption is wrong for `clang++`, which targets the same ABI through the GNU
driver and rejects `/W4` outright, so the whole of RT64 fails to compile. Using
the driver RT64 expects is simpler than patching the submodule.

Changing compiler invalidates the CMake cache, and the options do not survive
it. Delete the build directory when switching rather than reconfiguring in
place, or the build will silently come back with the runtime switched off.
