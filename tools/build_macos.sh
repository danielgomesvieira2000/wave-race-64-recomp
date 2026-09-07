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
for patch in patch_rt64.py patch_n64recomp.py patch_rsprecomp.py patch_librecomp.py patch_macos.py patch_water.py patch_runtime_shutdown.py; do
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
cmake -S . -B build-macos -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DWR64_WITH_RUNTIME=ON -DWR64_WITH_RECOMPILED=ON -DWR64_WITH_FRONTEND=ON
cmake --build build-macos --target WaveRace64Recomp -j "$WR64_JOBS"
python3 tools/package_macos.py build-macos/WaveRace64Recomp.app
echo "Built: $WR64_ROOT/build-macos/WaveRace64Recomp.app"
