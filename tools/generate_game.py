"""Turn a Wave Race 64 dump into the generated sources the port compiles.

This is the whole of phases 01, 02 and 05 in one command: disassemble the dump,
assemble it back into an ELF with symbols, check that ELF against the ROM, then
run N64Recomp and RSPRecomp over it. Everything it writes -- the disassembly,
`wr64.elf`, `RecompiledFuncs/` -- is derived from the dump and is not committed.
See docs/BUILDING.md for the step-by-step version, and docs/PORTING.md for why
each step is there.

The dump may be a `.z64`, `.n64` or `.v64`, or a ZIP containing exactly one of
those. Byte-swapped images are converted; anything that is not USA Rev A is
rejected here rather than producing failures later that look like tooling bugs.

Runs on Linux, macOS and Windows. The shell steps need bash and MIPS binutils,
which on Windows means WSL; on Linux and macOS they run natively.

    python tools/generate_game.py "Wave Race 64 (USA) (Rev A).z64"
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DECOMP = REPO / "reference" / "wr64-decomp"
RECOMPILER_DIR = REPO / "lib" / "N64ModernRuntime" / "N64Recomp" / "build-linux"

# The one revision every address in this project is tied to. See the README.
REV_A_SHA1 = "508dfc2d4caa42b6f6de5263d0aed5e44ac7966a"

# .n64 is byte-swapped in pairs, .v64 is word-swapped; .z64 is big-endian and
# is what everything downstream expects.
MAGIC_Z64 = bytes.fromhex("80371240")
MAGIC_N64 = bytes.fromhex("37804012")
MAGIC_V64 = bytes.fromhex("40123780")


def run(*args, cwd=REPO):
    print("+", " ".join(str(a) for a in args), flush=True)
    subprocess.run([str(a) for a in args], cwd=cwd, check=True)


def run_bash(script, cwd=REPO):
    """Run one of the tools/*.sh scripts, through WSL on Windows."""
    if os.name == "nt":
        # The scripts derive the repository root from their own path, so the
        # WSL-side path is whatever WSL sees this checkout as.
        run("wsl", "-d", "Ubuntu", "--", "bash", wsl_path(REPO / script), cwd=cwd)
    else:
        run("bash", REPO / script, cwd=cwd)


def wsl_path(path):
    out = subprocess.run(["wsl", "-d", "Ubuntu", "--", "wslpath", "-a", str(path)],
                         capture_output=True, text=True, check=True)
    return out.stdout.strip()


def read_rom(path: Path) -> bytes:
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as archive:
            entries = [n for n in archive.namelist()
                       if n.lower().endswith((".z64", ".n64", ".v64"))]
            if len(entries) != 1:
                sys.exit(f"The ZIP holds {len(entries)} ROMs; it must hold exactly one.")
            print(f"reading {entries[0]} from the archive")
            return archive.read(entries[0])
    return path.read_bytes()


def to_big_endian(data: bytes) -> bytes:
    head = data[:4]
    if head == MAGIC_Z64:
        return data
    if head == MAGIC_N64:
        print("byte-swapped (.n64) image: converting")
        return bytes(v for pair in zip(data[1::2], data[::2]) for v in pair)
    if head == MAGIC_V64:
        print("word-swapped (.v64) image: converting")
        return b"".join(data[i:i + 4][::-1] for i in range(0, len(data), 4))
    sys.exit("Not an N64 ROM: no recognised header magic.")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("rom", type=Path, help="Rev A .z64, .n64, .v64 or a ZIP holding one")
    args = parser.parse_args()

    if not args.rom.is_file():
        sys.exit(f"No such file: {args.rom}")

    data = to_big_endian(read_rom(args.rom))
    digest = hashlib.sha1(data).hexdigest()
    if digest != REV_A_SHA1:
        sys.exit(f"This is not Wave Race 64 (USA) (Rev A / v1.1).\n"
                 f"  sha1 of what you gave: {digest}\n"
                 f"  sha1 required:         {REV_A_SHA1}\n"
                 "The original US release (v1.0), the Japanese and European versions and\n"
                 "the Shindou edition are laid out differently and will not work.")
    print(f"dump verified: {digest}")

    if not DECOMP.is_dir():
        sys.exit("The reference decompilation is missing. Clone it first:\n"
                 "  git clone https://github.com/LLONSIT/Wave-Race-64.git reference/wr64-decomp")

    recompiler = RECOMPILER_DIR / "N64Recomp"
    rsprecomp = RECOMPILER_DIR / "RSPRecomp"
    if not recompiler.is_file() or not rsprecomp.is_file():
        sys.exit(f"The recompiler is not built. Run:\n"
                 f"  bash tools/wsl_build_recompiler.sh   (wsl bash ... on Windows)")

    # splat reads the dump from inside the decompilation checkout, under the
    # name its config names.
    (DECOMP / "baserom.us.rev1.z64").write_bytes(data)
    shutil.copy2(DECOMP / "baserom.us.rev1.z64", REPO / "wr64.us.rev1.z64")

    print("\n=== phase 01: disassemble, assemble, verify ===")
    run_bash("tools/wsl_build_all.sh")
    run_bash("tools/wsl_verify_elf.sh")
    shutil.copy2(DECOMP / "build" / "waverace64.us.rev1.elf", REPO / "wr64.elf")
    print(f"wrote {REPO / 'wr64.elf'}")

    print("\n=== phase 02: recompile the game ===")
    run_bash("tools/wsl_recompile.sh")
    for script in ("fix_overlay_relocs.py", "gen_runtime_func_table.py"):
        run(sys.executable, REPO / "tools" / script)

    print("\n=== phase 05: recompile the audio microcode ===")
    run_bash("tools/wsl_recompile_rsp.sh")

    print("\nGenerated sources are in RecompiledFuncs/. Configure and build next.")


if __name__ == "__main__":
    main()
