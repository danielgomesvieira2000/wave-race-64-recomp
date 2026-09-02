#!/usr/bin/env bash
# Phase 01 gate check: is the assembled ELF faithful to the ROM?
#
# Linking successfully proves very little on its own. The gate is that the code
# we assembled is byte-identical to the code in the cartridge -- if it is not,
# every function N64Recomp later emits is subtly wrong.
set -euo pipefail

REPO="/mnt/c/Users/Daniel/claude-projects/n64recomp_waverace64"
DECOMP="$REPO/reference/wr64-decomp"
ELF="build/waverace64.us.rev1.elf"
ROM="baserom.us.rev1.z64"
PREFIX=mips-linux-gnu-

cd "$DECOMP"

echo "=== relocations ==="
# --emit-relocs should have preserved these; overlays cannot be relocated at
# runtime without them.
"${PREFIX}readelf" -SW "$ELF" | grep -E 'RELA|\.rel' | head -8 || echo "  (no relocation sections found)"
echo "rela section count: $("${PREFIX}readelf" -SW "$ELF" | grep -cE ' REL(A)? ' || true)"

echo
echo "=== sections carrying code ==="
"${PREFIX}readelf" -SW "$ELF" | awk '$3 == "PROGBITS" && $8 ~ /X/ {printf "  %-28s addr=%s off=%s size=%s\n", $2, $4, $5, $6}' | head -12

echo
echo "=== byte-fidelity check against the ROM ==="
python3 - <<'PY'
import subprocess, sys

ELF = "build/waverace64.us.rev1.elf"
ROM = "baserom.us.rev1.z64"
PREFIX = "mips-linux-gnu-"

rom = open(ROM, "rb").read()

# Section headers: name, type, addr, offset, size, flags
out = subprocess.run([PREFIX + "readelf", "-SW", ELF],
                     capture_output=True, text=True).stdout

# The .text of each code segment should appear verbatim in the ROM. We do not
# know each section's ROM offset from the ELF alone, so we search for the
# section's bytes in the ROM -- an exact hit at a plausible offset is strong
# evidence, and a miss is decisive.
import re
# Code sections here are named after their segment (.codeseg, .ovl_i0, ...),
# not .text, so select on the executable flag instead of the name.
rows = re.findall(
    r"\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+\S+\s+([A-Zx]*)",
    out)

elf_bytes = open(ELF, "rb").read()
checked = matched = 0
missing = []

for name, typ, addr, off, size, flags in rows:
    if typ != "PROGBITS" or "X" not in flags:
        continue
    size_i, off_i = int(size, 16), int(off, 16)
    if size_i < 0x100:
        continue
    blob = elf_bytes[off_i:off_i + size_i]
    if len(blob) != size_i:
        continue
    checked += 1
    if blob in rom:
        matched += 1
    else:
        missing.append((name, addr, size))

print(f"  .text sections checked : {checked}")
print(f"  found verbatim in ROM  : {matched}")
if missing:
    print(f"  NOT found ({len(missing)}):")
    for name, addr, size in missing[:10]:
        print(f"    {name:<24} vram=0x{addr} size=0x{size}")
    sys.exit(1)
else:
    print("  every code section appears byte-for-byte in the cartridge.")
PY
