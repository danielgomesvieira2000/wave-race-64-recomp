#!/usr/bin/env bash
# Build SDL2, FreeType and libpng from pinned source, for a redistributable app.
#
# Why not just use Homebrew's. A Homebrew bottle is built for the OS it is
# installed on, so a bundle that carries one advertises whatever minimum that
# library requires -- which on an up-to-date machine is newer than the
# deployment target the port itself was compiled for, and the app then refuses
# to launch on the very systems it claims to support. tools/package_macos.py
# derives LSMinimumSystemVersion from the shipped binaries precisely so this
# does not pass unnoticed; this script is how to fix it.
#
# A normal local build does not need any of this: build_macos.sh without
# WR64_DEPENDENCY_PREFIX uses Homebrew's copies and works fine on the machine
# that built it.
#
#   bash tools/build_macos_dependencies.sh
#   WR64_BUILD_DIR=build-macos-release \
#   WR64_DEPENDENCY_PREFIX="$PWD/build-macos-deps/install" \
#       bash tools/build_macos.sh
#
# Environment:
#   WR64_MACOS_TARGET    deployment target to build against (default 15.0)
#   WR64_DEPS_BUILD_DIR  where to work (default build-macos-deps)
#   WR64_JOBS            parallelism
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "$(uname -s)" != Darwin ]; then
    echo "This script is for macOS." >&2
    exit 1
fi

if [ -z "${DEVELOPER_DIR:-}" ]; then
    for candidate in /Applications/Xcode.app/Contents/Developer \
                     /Applications/Xcode-beta.app/Contents/Developer; do
        if [ -d "$candidate" ]; then export DEVELOPER_DIR="$candidate"; break; fi
    done
fi

TARGET="${WR64_MACOS_TARGET:-15.0}"
JOBS="${WR64_JOBS:-$(sysctl -n hw.ncpu)}"
ARCH="$(uname -m)"
BUILD_DIR="${WR64_DEPS_BUILD_DIR:-$REPO/build-macos-deps}"
mkdir -p "$BUILD_DIR"
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
PREFIX="$BUILD_DIR/install"
SDK="$(xcrun --sdk macosx --show-sdk-path)"
mkdir -p "$PREFIX" "$BUILD_DIR"/{downloads,src,obj,logs}

export MACOSX_DEPLOYMENT_TARGET="$TARGET"

# Pinned by version and by checksum. These libraries end up inside a signed
# bundle that other people run; "whatever the mirror served today" is not a
# thing to build that from.
SDL2_VERSION=2.32.10
SDL2_URL="https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VERSION/SDL2-$SDL2_VERSION.tar.gz"
SDL2_SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165

PNG_VERSION=1.6.58
PNG_URL="https://downloads.sourceforge.net/project/libpng/libpng16/$PNG_VERSION/libpng-$PNG_VERSION.tar.xz"
PNG_SHA256=28eb403f51f0f7405249132cecfe82ea5c0ef97f1b32c5a65828814ae0d34775

FREETYPE_VERSION=2.14.3
FREETYPE_URL="https://downloads.sourceforge.net/project/freetype/freetype2/$FREETYPE_VERSION/freetype-$FREETYPE_VERSION.tar.xz"
FREETYPE_SHA256=36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f

fetch() {
    local name="$1" url="$2" checksum="$3"
    local archive="$BUILD_DIR/downloads/${url##*/}"
    if [ ! -f "$archive" ]; then
        curl -fL --retry 2 --connect-timeout 20 --max-time 300 -o "$archive" "$url"
    fi
    if [ "$(shasum -a 256 "$archive" | awk '{print $1}')" != "$checksum" ]; then
        echo "$name: checksum mismatch for $archive -- refusing to extract." >&2
        exit 1
    fi
    tar -xf "$archive" -C "$BUILD_DIR/src"
    echo "$name $checksum OK"
}

echo "=== sources ==="
fetch SDL2 "$SDL2_URL" "$SDL2_SHA256"
fetch libpng "$PNG_URL" "$PNG_SHA256"
fetch FreeType "$FREETYPE_URL" "$FREETYPE_SHA256"

# CMAKE_IGNORE_PREFIX_PATH keeps Homebrew out: finding a Homebrew library here
# is the exact failure this script exists to avoid, and CMake will happily do
# it. zlib comes from the SDK rather than being built, because macOS has always
# shipped one and it is in the list of libraries that may be linked from
# /usr/lib without bundling.
COMMON=(
    -G Ninja -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++
    "-DCMAKE_OSX_ARCHITECTURES=$ARCH"
    "-DCMAKE_OSX_DEPLOYMENT_TARGET=$TARGET"
    "-DCMAKE_OSX_SYSROOT=$SDK"
    "-DCMAKE_INSTALL_PREFIX=$PREFIX"
    "-DCMAKE_INSTALL_NAME_DIR=$PREFIX/lib"
    "-DCMAKE_PREFIX_PATH=$PREFIX"
    -DCMAKE_FIND_FRAMEWORK=LAST
    "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local;/opt/local"
)
ZLIB=("-DZLIB_LIBRARY=$SDK/usr/lib/libz.tbd" "-DZLIB_INCLUDE_DIR=$SDK/usr/include")

build_library() {
    local name="$1" source="$2"
    shift 2
    local obj="$BUILD_DIR/obj/$name"
    echo "=== $name ==="
    cmake "${COMMON[@]}" -S "$BUILD_DIR/src/$source" -B "$obj" "$@" \
        > "$BUILD_DIR/logs/$name-configure.log" 2>&1
    cmake --build "$obj" --parallel "$JOBS" > "$BUILD_DIR/logs/$name-build.log" 2>&1
    cmake --install "$obj" > "$BUILD_DIR/logs/$name-install.log" 2>&1
    echo "$name: installed"
}

build_library libpng "libpng-$PNG_VERSION" \
    -DPNG_SHARED=ON -DPNG_STATIC=OFF -DPNG_FRAMEWORK=OFF \
    -DPNG_TESTS=OFF -DPNG_TOOLS=OFF "${ZLIB[@]}"

build_library sdl2 "SDL2-$SDL2_VERSION" \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_TESTS=OFF \
    -DSDL_INSTALL_TESTS=OFF -DSDL2_DISABLE_SDL2MAIN=ON \
    -DSDL_HIDAPI_LIBUSB=OFF -DSDL_CCACHE=OFF

build_library freetype "freetype-$FREETYPE_VERSION" \
    -DBUILD_SHARED_LIBS=ON \
    -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON \
    -DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_PNG=ON \
    "-DPNG_LIBRARY_RELEASE=$PREFIX/lib/libpng16.dylib" \
    "-DPNG_PNG_INCLUDE_DIR=$PREFIX/include" "${ZLIB[@]}"

# ------------------------------------------------------------- licenses ----
# These get redistributed inside the app, so their notices travel with them.
LICENSES="$PREFIX/share/wr64-licenses"
mkdir -p "$LICENSES"/{SDL2,libpng,FreeType}
cp "$BUILD_DIR/src/SDL2-$SDL2_VERSION/LICENSE.txt"        "$LICENSES/SDL2/"
cp "$BUILD_DIR/src/libpng-$PNG_VERSION/LICENSE"           "$LICENSES/libpng/"
cp "$BUILD_DIR/src/freetype-$FREETYPE_VERSION/LICENSE.TXT" "$LICENSES/FreeType/"
cp "$BUILD_DIR/src/freetype-$FREETYPE_VERSION/docs/FTL.TXT" "$LICENSES/FreeType/"
cat > "$LICENSES/ACKNOWLEDGEMENTS.txt" <<'NOTICE'
This distribution uses SDL2, libpng and FreeType.

Portions of this software are copyright The FreeType Project
(https://freetype.org). All rights reserved. FreeType is distributed here
under the FreeType License (FTL); see FreeType/FTL.TXT.
NOTICE

# ------------------------------------------------------------- checking ----
# The point of the whole exercise is the deployment target, so it is checked
# rather than assumed. A library that came out newer than asked for would
# otherwise only be noticed by a player on an older macOS.
echo "=== verifying ==="
for lib in "$PREFIX"/lib/*.dylib; do
    [ -L "$lib" ] && continue
    arch="$(lipo -archs "$lib")"
    minos="$(otool -l "$lib" | awk '/^ *minos /{print $2}' | sort -u | tr '\n' ' ')"
    printf '  %-24s %-8s minos %s\n' "$(basename "$lib")" "$arch" "$minos"
    if [ "$arch" != "$ARCH" ] || [ "$minos" != "$TARGET " ]; then
        echo "unexpected architecture or deployment target in $lib" >&2
        exit 1
    fi
done

echo
echo "Dependency prefix: $PREFIX"
echo "Build the app against it with:"
echo "    WR64_BUILD_DIR=build-macos-release \\"
echo "    WR64_DEPENDENCY_PREFIX=\"$PREFIX\" \\"
echo "        bash tools/build_macos.sh"
