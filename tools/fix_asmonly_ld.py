"""Phase 01: repoint the three orphan data objects in the asm-only linker script.

splat derives a section subsegment's object path from its sibling text
subsegment. Three subsegments have no text sibling -- they are data-only files
in the decomp -- so splat falls back to src_path and emits:

    build/src/ovl_table.o(.data)
    build/src/libultra/vimodes.o(.data)
    build/src/libultra/libultra_bss.o(.bss)

The assembly for all three does get written, under asm-all/us/rev1/data/. Only
the linker script's idea of where the object lives is wrong. This rewrites those
references, and refuses to do so unless the corresponding .s actually exists, so
a silent mismatch cannot slip through.

Run from the repository root:
    python tools/fix_asmonly_ld.py
"""

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DECOMP = REPO / "reference" / "wr64-decomp"
LD = DECOMP / "linker_scripts" / "us" / "rev1" / "waverace64.asmonly.ld"
ASM_ROOT = DECOMP / "asm-all" / "us" / "rev1"

# object stem -> (asm-relative stem, section suffix splat used for the .s name)
REMAP = {
    "ovl_table": ("data/ovl_table", "data"),
    "libultra/vimodes": ("data/libultra/vimodes", "data"),
    "libultra/libultra_bss": ("data/libultra/libultra_bss", "bss"),
}


def prune_spurious_undefined_funcs():
    """Drop entries from splat's undefined_funcs_auto.ld that we actually define.

    A linker script assignment wins over an object's own definition and produces
    an ABS symbol -- one with no section. N64Recomp cannot resolve a call to an
    ABS symbol, because it has no way to know which section the target lives in,
    so a single stale entry here fails a whole function with a misleading "no
    function found for jal target".

    splat lists a function as undefined when it cannot attribute it, which is
    correct for the overlay entry points that genuinely live at an address
    several sections share. It is wrong for anything our assembly defines, and
    those are the entries removed here.
    """
    auto = LD.parent / "auto-asmonly" / "undefined_funcs_auto.ld"
    if not auto.exists():
        return

    defined = set()
    for source in ASM_ROOT.rglob("*.s"):
        for line in source.read_text(errors="replace").splitlines():
            m = re.match(r"glabel\s+([A-Za-z_]\w*)", line)
            if m:
                defined.add(m.group(1))

    kept, dropped = [], []
    for line in auto.read_text().splitlines(keepends=True):
        m = re.match(r"\s*([A-Za-z_]\w*)\s*=", line)
        if m and m.group(1) in defined:
            dropped.append(m.group(1))
        else:
            kept.append(line)

    if dropped:
        auto.write_text("".join(kept))
        print(f"\ndropped {len(dropped)} spurious undefined-function assignment(s) "
              f"that our objects define:")
        for name in dropped:
            print(f"  {name}")
    else:
        print("\nno spurious undefined-function assignments")


def place_bss_absolutely(text):
    """Pin each bss object to the address splat recorded for it.

    The same shortfall that affects data affects bss: a .bss object can be
    smaller than the span it represents, so the next one starts early and the
    error accumulates. Padding is the right answer for data, where the missing
    bytes are real cartridge contents that can be restored from the ROM. It is
    the wrong answer here, because bss objects do not tile a contiguous range --
    audio_bss, bss1 and the libultra bss files sit in different segments with
    gaps between them, so "pad to where the next one starts" would invent
    hundreds of kilobytes of nonsense.

    bss holds no data, only addresses, and splat annotates every symbol with the
    address it belongs at. Setting the location counter before each object uses
    that recorded address directly, so nothing can drift into anything else.
    """
    if ". = ABSOLUTE(0x" in text:
        print("\nbss objects already pinned; leaving the script alone")
        return text

    placed = 0
    out = []
    for line in text.splitlines(keepends=True):
        m = re.search(r"build/asm-all/us/rev1/(\S+)\.bss\.o\(\.bss\)", line)
        if m:
            source = ASM_ROOT / f"{m.group(1)}.bss.s"
            vram = first_vram(source)
            if vram is not None:
                indent = line[:len(line) - len(line.lstrip())]
                out.append(f"{indent}. = ABSOLUTE(0x{vram:08X});\n")
                placed += 1
        out.append(line)
    print(f"\npinned {placed} bss object(s) to their recorded addresses")
    return "".join(out)


def first_vram(path):
    if not path.exists():
        return None
    for line in path.read_text(errors="replace").splitlines():
        m = re.search(r"/\*\s*([0-9A-Fa-f]{8})\s*\*/", line)
        if m:
            return int(m.group(1), 16)
    return None


def main():
    if not LD.exists():
        sys.exit(f"missing {LD}\nRun tools/wsl_run_splat_asmonly.sh first.")

    text = LD.read_text()
    before = len(re.findall(r"build/src/[^\s)]*\.o", text))
    changed = 0

    for stem, (asm_stem, suffix) in REMAP.items():
        source = ASM_ROOT / f"{asm_stem}.{suffix}.s"
        if not source.exists():
            sys.exit(f"expected {source} to exist but it does not; refusing to "
                     f"repoint build/src/{stem}.o at an object that will never "
                     f"be built")
        old = f"build/src/{stem}.o"
        new = f"build/asm-all/us/rev1/{asm_stem}.{suffix}.o"
        if old in text:
            text = text.replace(old, new)
            changed += 1
            print(f"  {old}\n    -> {new}")

    text = place_bss_absolutely(text)
    prune_spurious_undefined_funcs()

    LD.write_text(text)
    after = len(re.findall(r"build/src/[^\s)]*\.o", text))

    print()
    print(f"rewrote {changed} reference(s)")
    print(f"build/src/*.o references: {before} -> {after}")
    if after:
        sys.exit(f"{after} C object reference(s) remain; the link will fail")
    print("linker script is now asm-only.")


if __name__ == "__main__":
    main()
