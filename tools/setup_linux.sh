#!/usr/bin/env bash
# One-time preparation for a Linux build: packages, the reference decompilation,
# and the Python environment splat runs in.
#
# Everything this installs is a build dependency of something in lib/, not of
# this project directly:
#
#   clang, lld            N64Recomp's output is validated against Clang; GCC's
#                         optimizer has miscompiled recompiled code.
#   libsdl2-dev           the window, the pad and the audio device.
#   libfreetype-dev       RmlUi's font engine, under recompui.
#   libgtk-3-dev          nativefiledialog-extended, which RT64 opens the ROM
#                         picker with. xdg-desktop-portal is the alternative
#                         upstream offers and is not the default.
#   libvulkan-dev         RT64's only renderer on Linux.
#   mesa-vulkan-drivers   a driver to actually run it. Skip on a machine with
#                         the proprietary NVIDIA or AMD stack already.
#   binutils-mips-linux-gnu   assembling the disassembly into an ELF.
#
# Nothing here is installed without asking: with no arguments the script prints
# what is missing and the command that installs it. Pass --install to run it.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV="${WR64_VENV:-$REPO/.venv}"
DECOMP="$REPO/reference/wr64-decomp"
# The revision the project's symbol work was done against.
DECOMP_REV=a51b38a2aaef68da10ea1e47247e70be3b1d4c70

PACKAGES=(
    clang lld cmake ninja-build pkg-config python3 python3-venv
    libsdl2-dev libfreetype-dev libgtk-3-dev
    libvulkan-dev vulkan-tools mesa-vulkan-drivers
    binutils-mips-linux-gnu
)

install=0
[ "${1:-}" = "--install" ] && install=1

# ------------------------------------------------------------- packages ----
if command -v dpkg-query > /dev/null; then
    missing=()
    for p in "${PACKAGES[@]}"; do
        if ! dpkg-query -W -f='${Status}' "$p" 2> /dev/null | grep -q "install ok installed"; then
            missing+=("$p")
        fi
    done

    if [ ${#missing[@]} -eq 0 ]; then
        echo "packages: all present"
    elif [ "$install" -eq 1 ]; then
        echo "installing: ${missing[*]}"
        sudo apt-get update
        sudo apt-get install -y "${missing[@]}"
    else
        echo "missing packages: ${missing[*]}"
        echo
        echo "Install them with:"
        echo "    sudo apt-get install ${missing[*]}"
        echo "or re-run this script as: bash tools/setup_linux.sh --install"
        echo
    fi
else
    echo "Not a dpkg-based distribution -- install the equivalents of these yourself:"
    printf '    %s\n' "${PACKAGES[@]}"
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

echo
echo "Ready. Next:"
echo "    bash tools/build_linux.sh /path/to/'Wave Race 64 (USA) (Rev A).z64'"
