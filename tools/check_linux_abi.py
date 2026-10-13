#!/usr/bin/env python3
"""Reject Linux release binaries whose glibc requirements exceed 2.42."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess


def check_glibc_requirements(version_info: str, maximum=(2, 42)):
    names = re.findall(r"\bName:\s+(GLIBC_[\w.]+)", version_info)
    if "GLIBC_PRIVATE" in names:
        raise ValueError("binary depends on GLIBC_PRIVATE")
    versions = [tuple(map(int, name.removeprefix("GLIBC_").split(".")))
                for name in names if re.fullmatch(r"GLIBC_\d+(?:\.\d+)+", name)]
    highest = max(versions, default=(0, 0))
    if highest > maximum:
        required = ".".join(map(str, highest))
        supported = ".".join(map(str, maximum))
        raise ValueError(f"binary requires glibc {required}; Linux releases target {supported} or earlier. "
                         "Rebuild on an older glibc baseline with WR64_LINUX_MATH_COMPAT=ON.")
    return highest


def check_binary_abi(binary: Path, maximum=(2, 42)):
    binary = Path(binary)
    with binary.open("rb") as stream:
        if stream.read(4) != b"\x7fELF":
            raise ValueError(f"not an ELF binary: {binary}")
    readelf = shutil.which("readelf") or shutil.which("llvm-readelf")
    if not readelf:
        raise ValueError("readelf is required to check Linux releases (install binutils)")
    environment = os.environ.copy()
    environment["LC_ALL"] = "C"
    result = subprocess.run([readelf, "--version-info", str(binary)], check=True,
                            capture_output=True, text=True, env=environment)
    return check_glibc_requirements(result.stdout, maximum)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    try:
        highest = check_binary_abi(args.binary)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"error: {error}\n")
    print("Linux glibc requirement:", ".".join(map(str, highest)), "(maximum allowed: 2.42)")


if __name__ == "__main__":
    main()
