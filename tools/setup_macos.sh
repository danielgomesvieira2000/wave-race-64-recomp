#!/usr/bin/env bash
# One-time preparation. SDL2 and FreeType must already be installed.
set -euo pipefail
WR64_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$WR64_ROOT"
git submodule update --init --recursive
if [[ ! -d reference/wr64-decomp ]]; then
    git clone https://github.com/LLONSIT/Wave-Race-64.git reference/wr64-decomp
    git -C reference/wr64-decomp checkout a51b38a2aaef68da10ea1e47247e70be3b1d4c70
fi
if [[ ! -d .venv ]]; then python3 -m venv .venv; fi
.venv/bin/pip install 'PyYAML==6.0.3' 'pylibyaml==0.1.0' 'tqdm==4.67.1' \
    'intervaltree==3.1.0' 'colorama==0.4.6' 'spimdisasm==1.42.4' \
    'rabbitizer==1.16.2' 'pygfxd==1.0.5' 'n64img==0.3.3' 'crunch64==0.6.2'
if ! command -v mips-linux-gnu-as >/dev/null && [[ ! -x build-toolchain/install/bin/mips-linux-gnu-as ]]; then
    mkdir -p build-toolchain
    cd build-toolchain
    curl -fL https://ftp.gnu.org/gnu/binutils/binutils-2.44.tar.xz -o binutils-2.44.tar.xz
    echo 'ce2017e059d63e67ddb9240e9d4ec49c2893605035cd60e92ad53177f4377237  binutils-2.44.tar.xz' | shasum -a 256 -c -
    tar -xf binutils-2.44.tar.xz
    mkdir -p obj
    cd obj
    ../binutils-2.44/configure --target=mips-linux-gnu --prefix="$WR64_ROOT/build-toolchain/install" \
        --disable-nls --disable-werror --disable-gdb --disable-gprofng --without-zstd --with-system-zlib
    make -j "${WR64_JOBS:-8}" MAKEINFO=true
    make install MAKEINFO=true
fi
