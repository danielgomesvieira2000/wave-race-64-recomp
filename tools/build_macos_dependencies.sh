#!/usr/bin/env bash
# Build release dylibs from checksum-pinned source, without Homebrew libraries.
set -euo pipefail
WR64_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ "$(uname -s)" != Darwin ]]; then
    echo "This dependency build requires macOS." >&2
    exit 1
fi
if [[ -z "${DEVELOPER_DIR:-}" ]]; then
    for candidate in /Applications/Xcode-beta.app/Contents/Developer /Applications/Xcode.app/Contents/Developer; do
        if [[ -d "$candidate" ]]; then export DEVELOPER_DIR="$candidate"; break; fi
    done
fi
WR64_MACOS_TARGET="${WR64_MACOS_TARGET:-15.0}"
WR64_JOBS="${WR64_JOBS:-4}"
WR64_DEPS_BUILD_DIR="${WR64_DEPS_BUILD_DIR:-$WR64_ROOT/build-macos-deps}"
mkdir -p "$WR64_DEPS_BUILD_DIR"
WR64_DEPS_BUILD_DIR="$(cd "$WR64_DEPS_BUILD_DIR" && pwd)"
WR64_DEPS_PREFIX="${WR64_DEPS_PREFIX:-$WR64_DEPS_BUILD_DIR/install}"
mkdir -p "$WR64_DEPS_PREFIX"
WR64_DEPS_PREFIX="$(cd "$WR64_DEPS_PREFIX" && pwd)"
WR64_SDK="$(xcrun --sdk macosx --show-sdk-path)"
WR64_NINJA="$(command -v ninja)"
export MACOSX_DEPLOYMENT_TARGET="$WR64_MACOS_TARGET"
mkdir -p "$WR64_DEPS_BUILD_DIR/"{downloads,src,obj,logs}
export WR64_ROOT WR64_DEPS_BUILD_DIR WR64_DEPS_PREFIX WR64_SDK WR64_NINJA WR64_MACOS_TARGET
export WR64_DEPS_COMMANDS="$WR64_DEPS_BUILD_DIR/commands.jsonl"
: > "$WR64_DEPS_COMMANDS"

record_command() {
    python3 - "$@" <<'PY'
import json, os, sys
replacements = [(os.environ[k], '${' + token + '}') for k, token in [
    ('WR64_DEPS_PREFIX', 'PREFIX'), ('WR64_SDK', 'SDK'),
    ('WR64_NINJA', 'NINJA'), ('WR64_DEPS_BUILD_DIR', 'BUILD_DIR'),
    ('WR64_ROOT', 'REPO')]]
args = sys.argv[1:]
for old, new in replacements:
    args = [arg.replace(old, new) for arg in args]
with open(os.environ['WR64_DEPS_COMMANDS'], 'a') as output:
    output.write(json.dumps(args) + '\n')
PY
}

fetch() {
    local name="$1" url="$2" checksum="$3" archive
    archive="$WR64_DEPS_BUILD_DIR/downloads/${url##*/}"
    if [[ ! -f "$archive" ]]; then
        record_command curl -fL --retry 2 --connect-timeout 20 --max-time 180 -o "$archive" "$url"
        curl -fL --retry 2 --connect-timeout 20 --max-time 180 -o "$archive" "$url"
    fi
    if [[ "$(shasum -a 256 "$archive" | awk '{print $1}')" != "$checksum" ]]; then
        echo "Checksum mismatch for $archive; refusing to extract." >&2
        exit 1
    fi
    record_command tar -xf "$archive" -C "$WR64_DEPS_BUILD_DIR/src"
    tar -xf "$archive" -C "$WR64_DEPS_BUILD_DIR/src"
    echo "$name: source SHA256 verified"
}

fetch SDL2 https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz \
    5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
fetch libpng https://downloads.sourceforge.net/project/libpng/libpng16/1.6.58/libpng-1.6.58.tar.xz \
    28eb403f51f0f7405249132cecfe82ea5c0ef97f1b32c5a65828814ae0d34775
fetch FreeType https://downloads.sourceforge.net/project/freetype/freetype2/2.14.3/freetype-2.14.3.tar.xz \
    36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f

common=(
    -G Ninja "-DCMAKE_MAKE_PROGRAM=$WR64_NINJA" -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++
    -DCMAKE_OSX_ARCHITECTURES=arm64 "-DCMAKE_OSX_DEPLOYMENT_TARGET=$WR64_MACOS_TARGET"
    "-DCMAKE_OSX_SYSROOT=$WR64_SDK" "-DCMAKE_INSTALL_PREFIX=$WR64_DEPS_PREFIX"
    "-DCMAKE_INSTALL_NAME_DIR=$WR64_DEPS_PREFIX/lib" "-DCMAKE_INSTALL_RPATH=$WR64_DEPS_PREFIX/lib"
    "-DCMAKE_PREFIX_PATH=$WR64_DEPS_PREFIX" -DCMAKE_FIND_FRAMEWORK=LAST
    '-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/opt/miniconda3;/Library/Frameworks/Mono.framework'
)
zlib=("-DZLIB_LIBRARY=$WR64_SDK/usr/lib/libz.tbd" "-DZLIB_INCLUDE_DIR=$WR64_SDK/usr/include")
build_library() {
    local name="$1" source="$2"
    shift 2
    local object="$WR64_DEPS_BUILD_DIR/obj/$name"
    record_command cmake "${common[@]}" -S "$WR64_DEPS_BUILD_DIR/src/$source" -B "$object" "$@"
    cmake "${common[@]}" -S "$WR64_DEPS_BUILD_DIR/src/$source" -B "$object" "$@" \
        > "$WR64_DEPS_BUILD_DIR/logs/$name-configure.log" 2>&1
    record_command cmake --build "$object" --parallel "$WR64_JOBS"
    cmake --build "$object" --parallel "$WR64_JOBS" > "$WR64_DEPS_BUILD_DIR/logs/$name-build.log" 2>&1
    record_command cmake --install "$object"
    cmake --install "$object" > "$WR64_DEPS_BUILD_DIR/logs/$name-install.log" 2>&1
    echo "$name: installed"
}
build_library libpng libpng-1.6.58 -DPNG_SHARED=ON -DPNG_STATIC=OFF -DPNG_FRAMEWORK=OFF \
    -DPNG_TESTS=OFF -DPNG_TOOLS=OFF "${zlib[@]}"
build_library sdl2 SDL2-2.32.10 -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF \
    -DSDL_TESTS=OFF -DSDL_INSTALL_TESTS=OFF -DSDL2_DISABLE_SDL2MAIN=ON -DSDL_HIDAPI_LIBUSB=OFF -DSDL_CCACHE=OFF
build_library freetype freetype-2.14.3 -DBUILD_SHARED_LIBS=ON -DFT_DISABLE_BZIP2=ON \
    -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_REQUIRE_ZLIB=ON -DFT_REQUIRE_PNG=ON \
    "-DPNG_LIBRARY_RELEASE=$WR64_DEPS_PREFIX/lib/libpng16.dylib" \
    "-DPNG_PNG_INCLUDE_DIR=$WR64_DEPS_PREFIX/include" "${zlib[@]}"

python3 - <<'PY'
import hashlib, json, os, re, shutil, subprocess
from pathlib import Path
root = Path(os.environ['WR64_DEPS_BUILD_DIR'])
prefix = Path(os.environ['WR64_DEPS_PREFIX'])
licenses = prefix / 'share/wr64-licenses'
sources = [
    ('SDL2', '2.32.10', 'SDL2-2.32.10', 'Zlib',
     'https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz',
     '5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165', ['LICENSE.txt']),
    ('libpng', '1.6.58', 'libpng-1.6.58', 'libpng-2.0',
     'https://downloads.sourceforge.net/project/libpng/libpng16/1.6.58/libpng-1.6.58.tar.xz',
     '28eb403f51f0f7405249132cecfe82ea5c0ef97f1b32c5a65828814ae0d34775', ['LICENSE']),
    ('FreeType', '2.14.3', 'freetype-2.14.3', 'FTL',
     'https://downloads.sourceforge.net/project/freetype/freetype2/2.14.3/freetype-2.14.3.tar.xz',
     '36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f',
     ['LICENSE.TXT', 'docs/FTL.TXT', 'docs/GPLv2.TXT', 'src/bdf/README', 'src/pcf/README']),
]
records = []
for name, version, directory, license_id, url, expected, notices in sources:
    archive = root / 'downloads' / url.rsplit('/', 1)[1]
    actual = hashlib.sha256(archive.read_bytes()).hexdigest()
    if actual != expected:
        raise SystemExit(f'{name}: source checksum mismatch')
    target = licenses / name
    target.mkdir(parents=True, exist_ok=True)
    copied = []
    for relative in notices:
        source = root / 'src' / directory / relative
        dest = target / relative.replace('/', '-')
        shutil.copyfile(source, dest)
        copied.append(str(dest.relative_to(prefix)))
    if name == 'FreeType':
        # Preserve the notices in the contributed source components named by LICENSE.TXT.
        for relative in ['src/base/fthash.c', 'include/freetype/internal/fthash.h',
                         'src/autofit/ft-hb-ft.c', 'src/autofit/ft-hb-decls.h',
                         'src/autofit/ft-hb-types.h', 'src/autofit/hb-script-list.h', 'src/gzip/zlib.h']:
            source = root / 'src' / directory / relative
            comment = re.match(r'\s*(?:/\*.*?\*/\s*)+', source.read_text(), re.S)
            if not comment:
                raise SystemExit(f'Missing license comment: {relative}')
            dest = target / (relative.replace('/', '-') + '-notice.txt')
            dest.write_text(comment.group(0).strip() + '\n')
            copied.append(str(dest.relative_to(prefix)))
    records.append({'name': name, 'version': version, 'source_url': url, 'source_sha256': actual,
                    'source_verified': True, 'license': license_id, 'license_files': copied})
(licenses / 'ACKNOWLEDGEMENTS.txt').write_text(
    'This distribution uses SDL2, libpng, and FreeType.\n'
    'Portions of this software are copyright \u00a9 2026 The FreeType Project '
    '(https://freetype.org). All rights reserved.\n'
    'FreeType is distributed here under the FreeType License (FTL).\n')
libraries = []
for binary in sorted((prefix / 'lib').glob('*.dylib')):
    if binary.is_symlink():
        continue
    arch = subprocess.check_output(['lipo', '-archs', str(binary)], text=True).strip()
    commands = subprocess.check_output(['otool', '-l', str(binary)], text=True)
    minima = re.findall(r'^\s*minos (\S+)', commands, re.M)
    if arch != 'arm64' or minima != [os.environ['WR64_MACOS_TARGET']]:
        raise SystemExit(f'Unexpected architecture/deployment target: {binary.name}: {arch}, {minima}')
    dependencies = []
    for line in subprocess.check_output(['otool', '-L', str(binary)], text=True).splitlines()[1:]:
        dep = line.strip().split(' (', 1)[0]
        if dep.startswith(('/System/Library/', '/usr/lib/')):
            dependencies.append(dep)
        elif dep.startswith(str(prefix) + '/') and Path(dep).is_file():
            dependencies.append('${PREFIX}/' + str(Path(dep).relative_to(prefix)))
        else:
            raise SystemExit(f'Unbundled dependency: {binary.name}: {dep}')
    libraries.append({'path': str(binary.relative_to(prefix)), 'architecture': arch,
                      'minimum_macos': minima[0], 'sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                      'dependencies': dependencies})
manifest = {'schema': 1, 'architecture': 'arm64', 'minimum_macos': os.environ['WR64_MACOS_TARGET'],
            'prefix': '${PREFIX}', 'build_directory': '${BUILD_DIR}',
            'sdk_version': subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-version'], text=True).strip(),
            'xcode': subprocess.check_output(['xcodebuild', '-version'], text=True).strip(),
            'sources': records, 'libraries': libraries, 'dependency_closure_verified': True,
            'commands': [json.loads(line) for line in Path(os.environ['WR64_DEPS_COMMANDS']).read_text().splitlines()],
            'options': {'shared': True, 'freetype_png': True, 'zlib': 'macOS system library',
                        'freetype_bzip2': False, 'freetype_harfbuzz': False, 'freetype_brotli': False},
            'validation_limit': 'Deployment targets and linked dependencies are verified; execution on macOS 15 requires a separate host test.'}
text = json.dumps(manifest, indent=2) + '\n'
if '/Users/' in text:
    raise SystemExit('Unexpected local user path in public dependency manifest')
(root / 'dependencies.json').write_text(text)
shutil.copyfile(root / 'dependencies.json', licenses / 'dependencies.json')
print('All installed dylibs are arm64, have the requested deployment target, and use only bundled/system dependencies.')
PY
echo "Dependency prefix: $WR64_DEPS_PREFIX"
echo "SDL2_DIR=$WR64_DEPS_PREFIX/lib/cmake/SDL2"
echo "FREETYPE_INCLUDE_DIR_ft2build=$WR64_DEPS_PREFIX/include/freetype2"
echo "FREETYPE_INCLUDE_DIR_freetype2=$WR64_DEPS_PREFIX/include/freetype2"
echo "FREETYPE_LIBRARY_RELEASE=$WR64_DEPS_PREFIX/lib/libfreetype.dylib"
