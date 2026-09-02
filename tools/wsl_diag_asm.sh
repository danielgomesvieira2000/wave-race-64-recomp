#!/usr/bin/env bash
# Diagnostic: why does assembling splat's output fail?
cd /mnt/c/Users/Daniel/claude-projects/n64recomp_waverace64/reference/wr64-decomp

F=asm-all/us/rev1/sys/sys_main.s

echo "=== file exists? ==="
ls -la "$F" || exit 1

echo
echo "=== first 15 lines ==="
head -15 "$F"

echo
echo "=== cpp step ==="
cpp -P -undef -Wundef -std=c99 -nostdinc \
    -D_MIPS_SZLONG=32 -DF3D_OLD -DNDEBUG -DMIPSEB -D_LANGUAGE_ASSEMBLY -D_ULTRA64 \
    -I include -I . -I include/libc -I include/PR -I bin -I src/libultra \
    -I "$(dirname "$F")" "$F" > /tmp/out.s
echo "cpp exit=$?"
echo "output lines: $(wc -l < /tmp/out.s)"

echo
echo "=== as step (errors shown) ==="
mips-linux-gnu-as -march=vr4300 -32 -G0 -EB -I include -I . -o /tmp/out.o /tmp/out.s 2>&1 | head -20
echo "as exit=${PIPESTATUS[0]}"

echo
echo "=== does macro.inc exist? ==="
find . -name "macro.inc" -not -path "./tools/*" | head
