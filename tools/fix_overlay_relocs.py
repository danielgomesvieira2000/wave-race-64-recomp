"""Phase 03: drop the relocation entries N64Recomp emits with no type.

N64Recomp casts an ELF relocation type straight to its own RelocType enum, which
covers R_MIPS_NONE through R_MIPS_GPREL16 -- values 0 to 7. Our ELF also carries
R_MIPS_PC16, which is type 10. Nothing validates the range, so the generated
recomp_overlays.inl ends up with entries reading

    { .offset = 0x00000CC4, ..., .type =  },

because the name lookup indexed past the end of an eight-entry table. Those do
not compile, and they are the only thing standing between a complete overlay
table and a working build.

Discarding them is correct, not a workaround. A PC-relative branch needs no
load-time fixup when its target is in the same section: relocating the section
moves the branch and its destination together, leaving the distance unchanged.
All 39 in this game were verified section-local before this tool was written,
and the check is repeated here rather than trusted, because an inter-section
PC16 would need real handling.

They exist at all because splat declares functions with `glabel`, making them
global, and GNU as emits a relocation for a branch to a global symbol even when
it resolves inside the same section.

Run from the repository root, after the recompiler:
    python tools/fix_overlay_relocs.py
"""

import re
import subprocess
import sys
from pathlib import Path

from toolchain import readelf_command

REPO = Path(__file__).resolve().parent.parent
INL = REPO / "RecompiledFuncs" / "recomp_overlays.inl"
ELF = "wr64.elf"
OVERLAY_RE = re.compile(r"ovl_|seg_1C3|segment_1B1FB0")


def readelf(flag):
    return subprocess.run(
        readelf_command() + [flag, ELF],
        capture_output=True, text=True, cwd=REPO).stdout


def verify_pc16_are_section_local():
    """Every untyped reloc should correspond to a section-local R_MIPS_PC16."""
    sections = {m.group(1): m.group(2)
                for m in re.finditer(r"\[\s*(\d+)\]\s+(\S+)", readelf("-SW"))}

    sym_section = {}
    for line in readelf("-sW").splitlines():
        m = re.match(r"\s*\d+:\s+[0-9a-f]+\s+\d+\s+\S+\s+\S+\s+\S+\s+(\S+)\s+(\S+)$", line)
        if m:
            sym_section.setdefault(m.group(2), m.group(1))

    current = None
    same = 0
    problems = []
    for line in readelf("-rW").splitlines():
        m = re.match(r"Relocation section '(\S+)'", line)
        if m:
            current = m.group(1)
            continue
        if not current or "R_MIPS_PC16" not in line:
            continue
        if not OVERLAY_RE.search(current):
            continue
        symbol = line.split()[-1]
        target_section = current.replace(".rel", "", 1)
        defined_in = sections.get(sym_section.get(symbol, ""), None)
        if defined_in == target_section:
            same += 1
        else:
            problems.append((target_section, symbol, defined_in))

    return same, problems


def main():
    if not INL.exists():
        sys.exit(f"missing {INL}\nRun the recompiler first.")

    text = INL.read_text()
    untyped = re.findall(r"^.*\.type =\s*\},.*$", text, re.M)
    if not untyped:
        print("no untyped relocation entries; nothing to do")
        return

    same, problems = verify_pc16_are_section_local()
    print(f"untyped relocation entries : {len(untyped)}")
    print(f"section-local PC16 relocs  : {same}")

    if problems:
        print("\nPC16 relocations whose target is NOT section-local:")
        for target_section, symbol, defined_in in problems[:10]:
            print(f"  {target_section:<18} {symbol:<26} defined in {defined_in}")
        sys.exit("refusing to discard: these need real relocation handling")

    if len(untyped) != same:
        sys.exit(f"refusing to discard: {len(untyped)} untyped entries but {same} "
                 f"section-local PC16 relocations -- they should correspond exactly")

    cleaned = re.sub(r"^.*\.type =\s*\},.*\n", "", text, flags=re.M)
    INL.write_text(cleaned)

    remaining = len(re.findall(r"^.*\.type =\s*\},.*$", cleaned, re.M))
    print(f"\ndiscarded {len(untyped)} no-op PC16 relocation entries")
    print(f"remaining untyped entries  : {remaining}")
    if remaining:
        sys.exit("entries remain; the build will still fail")


if __name__ == "__main__":
    main()
