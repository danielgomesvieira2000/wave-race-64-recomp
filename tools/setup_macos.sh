#!/usr/bin/env bash
# One-time preparation for a macOS build: the reference decompilation, the
# Python environment splat runs in, and MIPS binutils.
#
# Prerequisites this does not install, because they come from Xcode or Homebrew:
#
#   Xcode, with the Metal Toolchain component. RT64 compiles its shaders to
#   Metal with it. If the build later reports a missing Metal Toolchain:
#     DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
#         xcodebuild -downloadComponent MetalToolchain
#   CMake, Ninja, Python 3.10+:  brew install cmake ninja python
#   SDL2 and FreeType:           brew install sdl2 freetype
#
# MIPS binutils is the one thing with no Homebrew formula worth relying on, so
# it is built from source here, into build-toolchain/install. That takes a few
# minutes once and never again.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

VENV="${WR64_VENV:-$REPO/.venv}"
DECOMP="$REPO/reference/wr64-decomp"
# The revision the project's symbol work was done against.
DECOMP_REV=a51b38a2aaef68da10ea1e47247e70be3b1d4c70
JOBS="${WR64_JOBS:-$(sysctl -n hw.ncpu)}"

BINUTILS_VERSION=2.44
BINUTILS_SHA256=ce2017e059d63e67ddb9240e9d4ec49c2893605035cd60e92ad53177f4377237

if [ "$(uname -s)" != Darwin ]; then
    echo "This script is for macOS. On Linux use tools/setup_linux.sh." >&2
    exit 1
fi

missing=()
for tool in cmake ninja python3 clang; do
    command -v "$tool" > /dev/null || missing+=("$tool")
done
if ! pkg-config --exists sdl2 2> /dev/null && [ ! -d /opt/homebrew/include/SDL2 ] \
        && [ ! -d /usr/local/include/SDL2 ]; then
    missing+=("sdl2")
fi
if [ ${#missing[@]} -ne 0 ]; then
    echo "missing: ${missing[*]}"
    echo "Install them with: brew install cmake ninja python sdl2 freetype"
    echo
fi

# ------------------------------------------------------------ submodules ----
echo "=== submodules ==="
git -C "$REPO" submodule update --init --recursive

# ------------------------------- the reference decompilation, for splat ----
if [ ! -d "$DECOMP" ]; then
    echo "=== reference decompilation ==="
    # Not committed and not redistributed: it publishes no license, so it is
    # cloned on the builder's machine and everything derived from it stays
    # there. See docs/BUILDING.md.
    git clone https://github.com/LLONSIT/Wave-Race-64.git "$DECOMP"
    git -C "$DECOMP" checkout "$DECOMP_REV"
fi

# ------------------------------------------------ the python environment ----
echo "=== python environment for splat ==="
bash "$REPO/tools/wsl_setup_splat.sh"

# ------------------------------------------------------------- binutils ----
TOOLCHAIN="$REPO/build-toolchain/install"
if command -v mips-linux-gnu-as > /dev/null; then
    echo "=== MIPS binutils: already on PATH ==="
elif [ -x "$TOOLCHAIN/bin/mips-linux-gnu-as" ]; then
    echo "=== MIPS binutils: already built ==="
else
    echo "=== MIPS binutils $BINUTILS_VERSION (a few minutes, once) ==="
    mkdir -p "$REPO/build-toolchain"
    cd "$REPO/build-toolchain"
    tarball="binutils-$BINUTILS_VERSION.tar.xz"
    if [ ! -f "$tarball" ]; then
        curl -fL "https://ftp.gnu.org/gnu/binutils/$tarball" -o "$tarball"
    fi
    # The download is checked rather than trusted: this produces the assembler
    # every address in the port ultimately comes from.
    echo "$BINUTILS_SHA256  $tarball" | shasum -a 256 -c -
    tar -xf "$tarball"
    mkdir -p obj
    cd obj
    "../binutils-$BINUTILS_VERSION/configure" \
        --target=mips-linux-gnu \
        --prefix="$TOOLCHAIN" \
        --disable-nls --disable-werror --disable-gdb --disable-gprofng \
        --without-zstd --with-system-zlib
    # MAKEINFO=true skips the manuals, which need texinfo and are not wanted.
    make -j "$JOBS" MAKEINFO=true
    make install MAKEINFO=true
    cd "$REPO"
fi

echo
echo "Ready. Next:"
echo "    bash tools/build_macos.sh /path/to/'Wave Race 64 (USA) (Rev A).z64'"
