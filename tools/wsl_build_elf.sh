#!/usr/bin/env bash
# Phase 01 gate: assemble the disassembly into an ELF with symbols.
#
# This is an assembly-only build. It is not a matching decompilation and does
# not try to be: N64Recomp needs symbols, section metadata and relocations, all
# of which an asm-only link provides. No IDO, no decompiled C.
#
# Objects are derived from the linker script rather than by globbing, so we
# build exactly what the link consumes and a missing input fails loudly instead
# of silently producing a short ELF.
set -euo pipefail

# Derived rather than hardcoded: these scripts also run natively on Linux and
# macOS, where the checkout is not under /mnt/c.
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DECOMP="$REPO/reference/wr64-decomp"
LD_DIR="linker_scripts/us/rev1"
LD_SCRIPT="$LD_DIR/waverace64.asmonly.ld"
# Must match the paths the generated linker script emits, which are build/.
BUILD="build"
ELF="$BUILD/waverace64.us.rev1.elf"
GEN_LD="$LD_DIR/asmonly_undefined.ld"

PREFIX=mips-linux-gnu-
AS="${PREFIX}as"
LD="${PREFIX}ld"
OBJCOPY="${PREFIX}objcopy"
READELF="${PREFIX}readelf"

ASFLAGS="-march=vr4300 -32 -G0 -EB"
IINC="-I include -I . -I include/libc -I include/PR -I bin -I src/libultra"
DEFINES="-D_MIPS_SZLONG=32 -DF3D_OLD -DNDEBUG -DMIPSEB -D_LANGUAGE_ASSEMBLY -D_ULTRA64"
LDFLAGS="--no-check-sections --accept-unknown-input-arch --emit-relocs"

cd "$DECOMP"

if [ ! -f "$LD_SCRIPT" ]; then
    echo "missing $LD_SCRIPT -- run wsl_run_splat_asmonly.sh then fix_asmonly_ld.py" >&2
    exit 1
fi

# ---------------------------------------------------------------- objects ----
# read in a loop rather than with mapfile: macOS ships bash 3.2, which has no
# mapfile, and this script is now run natively there as well as under WSL.
OBJECTS=()
while IFS= read -r obj; do OBJECTS+=("$obj"); done \
    < <(grep -oE "build/[^ )]*\.o" "$LD_SCRIPT" | sort -u)
echo "objects referenced by the linker script: ${#OBJECTS[@]}"

built=0
failed=0
for obj in "${OBJECTS[@]}"; do
    stem="${obj#build/}"
    stem="${stem%.o}"
    mkdir -p "$(dirname "$obj")"

    if [ -f "$stem.s" ]; then
        # clang -E rather than cpp: macOS has no standalone cpp, and -x c is
        # needed because clang would otherwise treat a .s file as assembly and
        # preprocess it under different rules. Errors are no longer sent to
        # /dev/null: a failure here used to report only "FAILED to assemble".
        if ! clang -E -x c -P -undef -Wundef -std=c99 -nostdinc $DEFINES $IINC \
                 -I "$(dirname "$stem")" "$stem.s" \
             | $AS $ASFLAGS $IINC -I "$(dirname "$stem")" -o "$obj"; then
            echo "  FAILED to assemble $stem.s" >&2
            failed=$((failed + 1))
            continue
        fi
        built=$((built + 1))
    elif [ -f "$stem.bin" ]; then
        $OBJCOPY -I binary -O elf32-big "$stem.bin" "$obj"
        built=$((built + 1))
    else
        echo "  NO SOURCE for $obj" >&2
        failed=$((failed + 1))
    fi
done

echo "built $built object(s), $failed failure(s)"
if [ "$failed" -gt 0 ]; then
    echo "refusing to link with missing objects" >&2
    exit 1
fi

# ------------------------------------------------------- symbol resolution ---
# The decomp's C sources declare .bss and data symbols that no assembly file
# defines, so an asm-only link leaves them undefined. Their names encode their
# own addresses -- D_800DA9DC lives at 0x800DA9DC, D_i15_802C6E20 at
# 0x802C6E20 -- so they resolve mechanically.
#
# We link once to discover what is genuinely missing, define exactly those, and
# link again. Taking the list from the linker instead of guessing keeps it
# honest: anything whose name does not encode an address is reported, not hidden.
EXTRA_SCRIPTS=()
for f in "$LD_DIR/auto-asmonly/undefined_funcs_auto.ld" \
         "$LD_DIR/auto-asmonly/undefined_syms_auto.ld" \
         "$LD_DIR/libultra_undefined_syms.txt" \
         "$LD_DIR/resolve.txt"; do
    [ -f "$f" ] && EXTRA_SCRIPTS+=(-T "$f")
done

echo
echo "=== discovering undefined symbols ==="
: > "$GEN_LD"
set +e
DISCOVER=$($LD $LDFLAGS -T "$LD_SCRIPT" "${EXTRA_SCRIPTS[@]}" -o /dev/null 2>&1)
set -e

UNDEF=$(printf '%s\n' "$DISCOVER" \
        | sed -n "s/.*undefined reference to \`\([A-Za-z_][A-Za-z0-9_]*\)'.*/\1/p" \
        | sort -u)

# Symbols with real names -- __osRunningThread, gCurrentOptionsMenuItem -- carry
# no address in the name, but the decomp's symbol corpus knows where they live.
# Those files are already `name = 0xADDR;` assignments, i.e. valid linker script,
# so we look each missing symbol up rather than pasting the corpus wholesale,
# which would redefine symbols the objects already provide.
CORPUS=$(mktemp)
cat "$LD_DIR"/symbol_addrs.txt "$LD_DIR"/libultra_symbols.txt \
    "$LD_DIR"/ovl_symbols.txt "$LD_DIR"/audio_symbols.txt 2>/dev/null \
    | sed -n 's/^[[:space:]]*\([A-Za-z_][A-Za-z0-9_]*\)[[:space:]]*=[[:space:]]*\(0x[0-9A-Fa-f]*\).*/\1 \2/p' \
    > "$CORPUS"
echo "corpus symbols available   : $(wc -l < "$CORPUS")"

from_name=0
from_corpus=0
unresolved=""
for sym in $UNDEF; do
    addr=$(printf '%s' "$sym" | grep -oE '[0-9A-Fa-f]{8}$' || true)
    if [ -n "$addr" ]; then
        printf '%s = 0x%s;\n' "$sym" "$addr" >> "$GEN_LD"
        from_name=$((from_name + 1))
        continue
    fi
    corpus_addr=$(awk -v s="$sym" '$1 == s {print $2; exit}' "$CORPUS")
    if [ -n "$corpus_addr" ]; then
        printf '%s = %s;\n' "$sym" "$corpus_addr" >> "$GEN_LD"
        from_corpus=$((from_corpus + 1))
    else
        unresolved="$unresolved $sym"
    fi
done
rm -f "$CORPUS"

total=$(printf '%s\n' "$UNDEF" | grep -c . || true)
echo "undefined symbols reported : $total"
echo "resolved from their names   : $from_name"
echo "resolved from the corpus    : $from_corpus"
if [ -n "$unresolved" ]; then
    echo "STILL UNRESOLVED           :$unresolved"
fi

EXTRA_SCRIPTS+=(-T "$GEN_LD")

# ------------------------------------------------------------------- link ----
echo
echo "=== link ==="
$LD $LDFLAGS -T "$LD_SCRIPT" "${EXTRA_SCRIPTS[@]}" \
    -Map "$BUILD/waverace64.map" -o "$ELF"

echo "linked: $ELF"
ls -la "$ELF"

echo
echo "=== ELF contents ==="
echo "FUNC symbols  : $($READELF -sW "$ELF" | grep -c ' FUNC ' || true)"
echo "OBJECT symbols: $($READELF -sW "$ELF" | grep -c ' OBJECT ' || true)"
echo "sections      : $($READELF -SW "$ELF" | grep -cE '^\s*\[' || true)"
echo "relocations   : $($READELF -rW "$ELF" 2>/dev/null | grep -c '^0' || true)"
