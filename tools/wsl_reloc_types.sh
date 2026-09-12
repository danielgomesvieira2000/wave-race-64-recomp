#!/usr/bin/env bash
# Diagnostic: which relocation types does our ELF actually contain?
#
# N64Recomp casts the raw ELF relocation type straight to its RelocType enum,
# which only covers 0..7. Anything above that indexes past its name table and is
# emitted as an empty .type field in recomp_overlays.inl, which then fails to
# compile. This reports what is present so the offenders can be identified
# rather than guessed at.
set -euo pipefail

cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "=== relocation types across the whole ELF ==="
mips-linux-gnu-readelf -rW wr64.elf | awk '$3 ~ /^R_MIPS/ { print $3 }' | sort | uniq -c | sort -rn

echo
echo "=== types appearing in the overlay sections only ==="
mips-linux-gnu-readelf -rW wr64.elf | awk '
    /^Relocation section/ { sec = $3 }
    $3 ~ /^R_MIPS/ && sec ~ /ovl_|seg_1C3|segment_1B1FB0/ { print $3 }
' | sort | uniq -c | sort -rn
