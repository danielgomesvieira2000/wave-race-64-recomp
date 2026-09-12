#!/usr/bin/env bash
# Phase 01: the whole pipeline, ROM to verified ELF, in order.
#
# The order matters and two steps are not obvious:
#
#   pad_data_objects runs BEFORE the first build, because it works from the
#   config and can measure sources on its own.
#
#   pad_segment_tails runs AFTER a build, because the bytes it restores belong
#   to no subsegment at all -- the only way to see them is to compare the linked
#   ELF against the declared segment sizes. That is why the build runs twice.
#
# Re-running splat regenerates the sources and discards all padding, so the
# padding passes must follow it every time.
set -euo pipefail

# Derived rather than hardcoded: these scripts also run natively on Linux and
# macOS, where the checkout is not under /mnt/c.
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="$REPO/tools"
# The in-tree .venv the setup scripts make, unless one is named explicitly.
# $HOME/wr64venv is still honoured for checkouts set up before this.
VENV="${WR64_VENV:-$REPO/.venv}"
if [ ! -d "$VENV" ] && [ -d "$HOME/wr64venv" ]; then VENV="$HOME/wr64venv"; fi
DECOMP="$REPO/reference/wr64-decomp"

echo "=== 1/6 disassemble ==="
bash "$T/wsl_run_splat_asmonly.sh" | grep -E 'function labels|linker script'

echo
echo "=== 2/6 pad subsegments (from the config) ==="
cd "$DECOMP" && "$VENV/bin/python" "$T/pad_data_objects.py"

echo
echo "=== 3/6 repoint orphan objects and pin bss ==="
python3 "$T/fix_asmonly_ld.py" 2>/dev/null || "$VENV/bin/python" "$T/fix_asmonly_ld.py"

echo
echo "=== 4/6 build ==="
bash "$T/wsl_build_elf.sh" | tail -6

echo
echo "=== 5/6 pad segment tails (needs the built ELF) ==="
cd "$DECOMP" && "$VENV/bin/python" "$T/pad_segment_tails.py"

echo
echo "=== 6/6 rebuild ==="
bash "$T/wsl_build_elf.sh" | tail -6
