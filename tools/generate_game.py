#!/usr/bin/env python3
"""Generate native sources from the user's verified USA Rev A dump."""

import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parent.parent
DECOMP = ROOT / "reference/wr64-decomp"


def run(*args, cwd=ROOT):
    print("Running:", *map(str, args), flush=True)
    subprocess.run(list(map(str, args)), cwd=cwd, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path, help="Rev A .z64, .n64, .v64 or ZIP")
    args = parser.parse_args()
    if zipfile.is_zipfile(args.rom):
        with zipfile.ZipFile(args.rom) as archive:
            entries = [n for n in archive.namelist()
                       if n.lower().endswith((".z64", ".n64", ".v64"))]
            if len(entries) != 1:
                sys.exit("The ZIP must contain exactly one ROM.")
            data = archive.read(entries[0])
    else:
        data = args.rom.read_bytes()
    if data[:4] == bytes.fromhex("37804012"):
        data = bytes(v for pair in zip(data[1::2], data[::2]) for v in pair)
    elif data[:4] == bytes.fromhex("40123780"):
        data = b"".join(data[i:i + 4][::-1] for i in range(0, len(data), 4))
    if hashlib.sha1(data).hexdigest() != "508dfc2d4caa42b6f6de5263d0aed5e44ac7966a":
        sys.exit("ROM does not match Wave Race 64 (USA) (Rev A / v1.1).")
    if not DECOMP.is_dir():
        sys.exit("Clone https://github.com/LLONSIT/Wave-Race-64.git into reference/wr64-decomp first.")
    (DECOMP / "baserom.us.rev1.z64").write_bytes(data)
    run(sys.executable, ROOT / "tools/make_asm_only_yaml.py")
    config = "wr64.us.rev1.asm.yaml"
    shutil.copy2(ROOT / "recomp" / config, DECOMP / config)
    (DECOMP / "linker_scripts/us/rev1/auto-asmonly").mkdir(parents=True, exist_ok=True)
    run(sys.executable, "tools/splat/split.py", config, "--disassemble-all", cwd=DECOMP)
    run(sys.executable, ROOT / "tools/pad_data_objects.py")
    run(sys.executable, ROOT / "tools/fix_asmonly_ld.py")
    run("bash", ROOT / "tools/wsl_build_elf.sh")
    run(sys.executable, ROOT / "tools/pad_segment_tails.py")
    run("bash", ROOT / "tools/wsl_build_elf.sh")
    run("bash", ROOT / "tools/wsl_verify_elf.sh")
    shutil.copy2(DECOMP / "build/waverace64.us.rev1.elf", ROOT / "wr64.elf")
    run(ROOT / "build-tools/N64Recomp", "recomp/wr64.toml")
    for script in ("gen_reimplemented_decls.py", "fix_overlay_relocs.py", "gen_runtime_func_table.py"):
        run(sys.executable, ROOT / "tools" / script)
    run(ROOT / "build-tools/RSPRecomp", "recomp/aspMain.rsp.toml")
    print("Game and audio sources generated from the verified dump.")


if __name__ == "__main__":
    main()
