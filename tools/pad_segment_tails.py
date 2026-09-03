"""Phase 01 fix, second pass: pad each segment out to its declared ROM length.

pad_data_objects.py works per subsegment, from the config. That fixes the bulk
of the shortfall but cannot see one case: bytes at the very end of a segment
that belong to no subsegment at all. Every overlay ends that way, together
exactly 0x610 short, which is what pushed the ROM addresses of the four audio
segments out and left 22 bytes wrong in main_segment.

This pass measures the built ELF against the declared segment sizes and appends
whatever is missing, taken from the cartridge, to the object the linker script
uses for that segment's last section. It therefore has to run after a build,
and the build has to be repeated afterwards.

Run inside WSL from the decomp checkout:
    ~/wr64venv/bin/python tools/pad_segment_tails.py
"""

import re
import subprocess
from pathlib import Path

import yaml

DECOMP = Path("/mnt/c/Users/Daniel/claude-projects/n64recomp_waverace64/reference/wr64-decomp")
CONFIG = DECOMP / "wr64.us.rev1.asm.yaml"
LD = DECOMP / "linker_scripts/us/rev1/waverace64.asmonly.ld"
ELF = DECOMP / "build/waverace64.us.rev1.elf"
ROM_NAME = "baserom.us.rev1.z64"
MARKER = "segment tail restored by tools/pad_segment_tails.py"


def declared_spans():
    cfg = yaml.safe_load(CONFIG.read_text())
    segs = []
    for s in cfg["segments"]:
        if isinstance(s, dict) and isinstance(s.get("start"), int):
            segs.append((s["start"], s.get("name") or s.get("type")))
        elif isinstance(s, list) and s and isinstance(s[0], int):
            segs.append((s[0], "(end)"))
    segs.sort()
    return {nm: (st, segs[i + 1][0] - st)
            for i, (st, nm) in enumerate(segs) if i + 1 < len(segs)}


def elf_sections():
    out = subprocess.run(["mips-linux-gnu-readelf", "-SW", str(ELF)],
                         capture_output=True, text=True).stdout
    return {m.group(1).lstrip("."): int(m.group(5), 16)
            for m in re.finditer(
                r"\[\s*\d+\]\s+(\S+)\s+(PROGBITS)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", out)}


def last_object_for(segment):
    """The source file backing the last section the linker script pulls in."""
    text = LD.read_text()
    # Section headers look like `.ovl_i0 0x802C5800 : AT(ovl_i0_ROM_START) ...`,
    # so the name is followed by an address rather than a colon. Requiring
    # whitespace after the name also stops `.ovl_i1` matching `.ovl_i15`.
    block = re.search(rf"\.{re.escape(segment)}\s[^\n]*\n\s*\{{(.*?)\n\s*\}}",
                      text, re.S)
    if not block:
        return None
    refs = re.findall(r"build/asm-all/us/rev1/(\S+?)\.o\((\.\w+)\)", block.group(0))
    for stem, section in reversed(refs):
        if section in (".rodata", ".data", ".text"):
            path = DECOMP / "asm-all/us/rev1" / f"{stem}.s"
            if path.exists():
                return path, section
    return None


def main():
    spans = declared_spans()
    sizes = elf_sections()
    flags = {".data": '"wa"', ".rodata": '"a"', ".text": '"ax"'}

    padded = 0
    total = 0
    for name, (start, expected) in sorted(spans.items()):
        actual = sizes.get(name)
        if actual is None or actual >= expected:
            continue
        target = last_object_for(name)
        if not target:
            print(f"  {name}: short {expected - actual:#x} but no object found")
            continue
        path, section = target
        if MARKER in path.read_text(errors="replace"):
            continue
        deficit = expected - actual
        rom_off = start + actual
        with open(path, "a") as fh:
            fh.write(f"\n/* {MARKER}: {deficit:#x} bytes at ROM {rom_off:#x} */\n")
            fh.write(f".section {section}, {flags[section]}\n")
            fh.write(f'.incbin "{ROM_NAME}", {rom_off:#x}, {deficit:#x}\n')
        print(f"  {name:<18} +{deficit:#6x} -> {path.relative_to(DECOMP)} ({section})")
        padded += 1
        total += deficit

    print(f"\npadded {padded} segment tail(s), {total:#x} bytes = {total}")


if __name__ == "__main__":
    main()
