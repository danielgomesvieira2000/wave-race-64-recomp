"""Phase 01 fix: pad each generated object's data sections to their true length.

splat emits a subsegment only as far as its last symbol. Cartridge bytes past
that symbol are dropped, nothing realigns the next object, and the error
accumulates -- measured at 0x3080 by the end of main_segment, which dragged
.bss and the boot stack pointer down with it. See docs/findings/phase-01.md.

Three things this has to get right:

  Fill from the ROM, not with zeros. The dropped bytes are real cartridge
  contents and are not reliably zero. `.incbin` of the exact ROM range restores
  them byte for byte; `.space` would invent zeros and produce an ELF that looks
  padded while being wrong.

  Pad the section, not the file. Because rodata is migrated into function
  objects, one .s can carry .text, .data and .rodata at once, and the linker
  script pulls those sections out separately. Appending at end-of-file would put
  bytes in whichever section happened to be open. Each append therefore reopens
  its target section explicitly.

  Take lengths from the config, not from a guess about alignment. A subsegment
  runs from its own ROM start to the next declared start, so no assumption about
  what the original build aligned to is needed.

Run inside WSL from the decomp checkout, with the venv python:
    ~/wr64venv/bin/python tools/pad_data_objects.py
"""

import re
import subprocess
import tempfile
from collections import defaultdict
from pathlib import Path

import yaml

DECOMP = Path("/mnt/c/Users/Daniel/claude-projects/n64recomp_waverace64/reference/wr64-decomp")
CONFIG = DECOMP / "wr64.us.rev1.asm.yaml"
ASM_ROOT = DECOMP / "asm-all/us/rev1"
ROM_NAME = "baserom.us.rev1.z64"

AS = "mips-linux-gnu-as"
READELF = "mips-linux-gnu-readelf"
ASFLAGS = ["-march=vr4300", "-32", "-G0", "-EB"]
IINC = ["-I", "include", "-I", ".", "-I", "include/libc", "-I", "include/PR",
        "-I", "bin", "-I", "src/libultra"]
DEFINES = ["-D_MIPS_SZLONG=32", "-DF3D_OLD", "-DNDEBUG", "-DMIPSEB",
           "-D_LANGUAGE_ASSEMBLY", "-D_ULTRA64"]

MARKER = "padding restored by tools/pad_data_objects.py"
SECTION_FLAGS = {".data": '"wa"', ".rodata": '"a"', ".text": '"ax"'}


def walk_subsegments(config):
    """Yield (start, type, name) for every subsegment, in declaration order."""
    def walk(subs):
        for s in subs or []:
            if isinstance(s, list) and s and isinstance(s[0], int):
                yield (s[0], str(s[1]) if len(s) > 1 else None,
                       s[2] if len(s) > 2 else None)
            elif isinstance(s, dict):
                if isinstance(s.get("start"), int):
                    yield s["start"], s.get("type"), s.get("name")
                yield from walk(s.get("subsegments"))

    for seg in config.get("segments", []):
        if isinstance(seg, dict):
            if isinstance(seg.get("start"), int):
                yield seg["start"], seg.get("type"), seg.get("name")
            yield from walk(seg.get("subsegments"))
        elif isinstance(seg, list) and seg and isinstance(seg[0], int):
            yield seg[0], str(seg[1]) if len(seg) > 1 else None, None


def resolve(kind, name, start):
    """Map a subsegment to (source file, section name), or None."""
    if kind in (".data", "data"):
        section = ".data"
    elif kind in (".rodata", "rodata"):
        section = ".rodata"
    elif kind in ("asm", "hasm"):
        # Text is truncated the same way: bytes after the last function are not
        # emitted. Harmless in main_segment and codeseg, which happen to end on
        # a function, but every overlay ends short -- together exactly 0x610,
        # which is what pushed the audio segments' ROM addresses out.
        section = ".text"
    else:
        return None

    # Order matters, and the right order is the one the LINKER SCRIPT uses, not
    # the one the filenames suggest. With rodata migration on, splat writes
    # standalone <name>.rodata.s files but generates a script that takes .rodata
    # from the text object -- so the text object is what must be padded, and the
    # standalone file is dead weight. .data, confusingly, does come from the
    # standalone file. Preferring the sibling file for both looks tidier and
    # silently pads objects that are never linked.
    candidates = []
    if name:
        candidates.append(ASM_ROOT / f"{name}.s")
        if section != ".text":
            candidates.append(ASM_ROOT / "data" / f"{name}{section}.s")
    candidates.append(ASM_ROOT / "data" / f"{start:X}{section}.s")

    for path in candidates:
        if path.exists():
            return path, section
    return None


def section_sizes(path):
    """Assemble and report the size of each allocated section."""
    with tempfile.NamedTemporaryFile(suffix=".o", delete=False) as tmp:
        obj = tmp.name
    try:
        pre = subprocess.run(
            ["cpp", "-P", "-undef", "-std=c99", "-nostdinc"] + DEFINES + IINC
            + ["-I", str(path.parent), str(path)],
            cwd=DECOMP, capture_output=True, text=True)
        if pre.returncode != 0:
            return None
        asm = subprocess.run(
            [AS] + ASFLAGS + IINC + ["-I", str(path.parent), "-o", obj],
            cwd=DECOMP, input=pre.stdout, capture_output=True, text=True)
        if asm.returncode != 0:
            return None
        out = subprocess.run([READELF, "-SW", obj], cwd=DECOMP,
                             capture_output=True, text=True).stdout
        sizes = {}
        for m in re.finditer(
                r"\[\s*\d+\]\s+(\S+)\s+PROGBITS\s+[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)", out):
            sizes[m.group(1)] = int(m.group(2), 16)
        return sizes
    finally:
        Path(obj).unlink(missing_ok=True)


def main():
    config = yaml.safe_load(CONFIG.read_text())
    subs = list(walk_subsegments(config))
    boundaries = sorted({s for s, _, _ in subs})

    # Group by target so several subsegments feeding one section sum correctly.
    wanted = defaultdict(int)
    origin = {}
    for start, kind, name in subs:
        target = resolve(kind, name, start)
        if not target:
            continue
        nxt = next((b for b in boundaries if b > start), None)
        if nxt is None:
            continue
        wanted[target] += nxt - start
        origin.setdefault(target, start)

    print(f"sections to check: {len(wanted)}")

    padded = exact = short_fixed = skipped = 0
    total = 0

    for (path, section), expected in sorted(wanted.items()):
        text = path.read_text(errors="replace")
        if MARKER in text and section in text.split(MARKER, 1)[1]:
            continue

        sizes = section_sizes(path)
        if sizes is None:
            skipped += 1
            continue
        actual = sizes.get(section, 0)

        if actual == expected:
            exact += 1
            continue
        if actual > expected:
            skipped += 1
            continue

        deficit = expected - actual
        rom_off = origin[(path, section)] + actual
        with open(path, "a") as fh:
            fh.write(f"\n/* {MARKER}: {deficit:#x} bytes at ROM {rom_off:#x} "
                     f"that no symbol covers */\n")
            fh.write(f".section {section}, {SECTION_FLAGS[section]}\n")
            fh.write(f'.incbin "{ROM_NAME}", {rom_off:#x}, {deficit:#x}\n')
        padded += 1
        total += deficit

    print(f"  already exact : {exact}")
    print(f"  padded        : {padded}  ({total:#x} bytes = {total} restored)")
    print(f"  skipped       : {skipped}")


if __name__ == "__main__":
    main()
