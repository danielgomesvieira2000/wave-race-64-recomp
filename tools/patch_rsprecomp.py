"""Make RSPRecomp's indirect jumps ignore the low two bits of the target, as the
RSP itself does.

The RSP's program counter is twelve bits wide and instructions are word
aligned, so `jr` discards the low two bits of its register: the hardware target
of 0x12EF is 0x12EC. RSPRecomp's generated dispatch does not, and switches on
the raw value:

    switch ((jump_target | 0x1000) & 0x1FFF)

That is fine as long as every jump target is already aligned, which is why it
has gone unnoticed. Wave Race 64's audio microcode dispatches each command
through a table of halfwords in DMEM, and one of those entries is 0x12EF at the
point the game uses it -- the low bits carry something the hardware simply
throws away. The recompiled microcode instead fell through the switch, returned
UnhandledJumpTarget, and librecomp asserted and left the RSP task thread dead,
which stops the game: no further task of either kind is ever started.

Masking with 0x1FFC in the switch restores the hardware's behaviour for every
microcode, aligned targets included.

Scripted and idempotent because it patches a submodule: a submodule update would
otherwise revert it silently and the port would go quiet again with no obvious
cause.

Run from the repository root, then rebuild the recompiler and regenerate:
    python tools/patch_rsprecomp.py
    wsl bash tools/wsl_build_recompiler.sh
    wsl bash tools/wsl_recompile_rsp.sh
"""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TARGET = REPO / "lib" / "N64ModernRuntime" / "N64Recomp" / "RSPRecomp" / "src" / "rsp_recomp.cpp"

ANCHOR = '"    switch ((jump_target | 0x1000) & {:#X}) {{ \\n", rsp_mem_mask);'
REPLACEMENT = (
    '"    switch ((jump_target | 0x1000) & {:#X}) {{ \\n", rsp_mem_mask & ~3u);'
)


def main():
    if not TARGET.exists():
        sys.exit(f"missing {TARGET}\nRun: git submodule update --init --recursive")

    text = TARGET.read_text()

    if "rsp_mem_mask & ~3u" in text:
        print(f"  {TARGET.name} already patched")
        return

    if ANCHOR not in text:
        sys.exit(f"anchor not found in {TARGET}; upstream has changed and this "
                 f"patch needs revisiting")

    TARGET.write_text(text.replace(ANCHOR, REPLACEMENT, 1))
    print(f"  {TARGET.name} patched")
    print("\nRebuild the recompiler and regenerate the microcode to pick it up.")


if __name__ == "__main__":
    main()
