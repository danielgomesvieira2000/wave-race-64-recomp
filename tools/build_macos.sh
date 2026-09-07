#!/usr/bin/env bash
# Build the native app. On the first build, pass a USA Rev A ROM or ZIP.
set -euo pipefail
WR64_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$WR64_ROOT"
if [[ "$(uname -s)" != Darwin ]]; then
    echo "This build script requires macOS." >&2
    exit 1
fi
if [[ -z "${DEVELOPER_DIR:-}" ]]; then
    for candidate in /Applications/Xcode.app/Contents/Developer /Applications/Xcode-beta.app/Contents/Developer; do
        if [[ -d "$candidate" ]]; then export DEVELOPER_DIR="$candidate"; break; fi
    done
fi
export PATH="$WR64_ROOT/build-toolchain/install/bin:$WR64_ROOT/.venv/bin:$PATH"
WR64_JOBS="${WR64_JOBS:-8}"
WR64_BUILD_DIR="${WR64_BUILD_DIR:-build-macos}"
WR64_CMAKE_ARGS=(-DCMAKE_BUILD_TYPE=Release)
if [[ -n "${WR64_DEPENDENCY_PREFIX:-}" ]]; then
    WR64_CMAKE_ARGS+=(
        "-DCMAKE_PREFIX_PATH=$WR64_DEPENDENCY_PREFIX"
        "-DSDL2_DIR=$WR64_DEPENDENCY_PREFIX/lib/cmake/SDL2"
        "-DFREETYPE_INCLUDE_DIR_freetype2=$WR64_DEPENDENCY_PREFIX/include/freetype2"
        "-DFREETYPE_INCLUDE_DIR_ft2build=$WR64_DEPENDENCY_PREFIX/include/freetype2"
        "-DFREETYPE_LIBRARY_RELEASE=$WR64_DEPENDENCY_PREFIX/lib/libfreetype.dylib"
    )
fi
for patch in patch_rt64.py patch_n64recomp.py patch_rsprecomp.py patch_librecomp.py patch_macos.py patch_water.py patch_runtime_shutdown.py patch_texture_packs.py; do
    python3 "tools/$patch"
done
cmake -S lib/N64ModernRuntime/N64Recomp -B build-tools -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-tools --target N64RecompCLI RSPRecomp -j "$WR64_JOBS"
if [[ $# -gt 0 ]]; then
    python3 tools/generate_game.py "$1"
elif [[ ! -f RecompiledFuncs/aspMain_rsp.cpp ]]; then
    echo 'First build: bash tools/build_macos.sh "/path/to/Wave Race 64 (USA) (Rev A).zip"' >&2
    exit 1
fi
cmake -S . -B "$WR64_BUILD_DIR" -G Ninja \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${WR64_MACOS_TARGET:-15.0}" \
    "${WR64_CMAKE_ARGS[@]}" \
    -DWR64_WITH_RUNTIME=ON -DWR64_WITH_RECOMPILED=ON -DWR64_WITH_FRONTEND=ON
cmake --build "$WR64_BUILD_DIR" --target WaveRace64Recomp -j "$WR64_JOBS"
python3 tools/package_macos.py "$WR64_BUILD_DIR/WaveRace64Recomp.app"
echo "Built: $WR64_BUILD_DIR/WaveRace64Recomp.app"
