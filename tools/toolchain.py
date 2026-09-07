"""Select native MIPS binutils, retaining the Windows WSL fallback."""

import os
import shutil


def readelf_command():
    tool = shutil.which("mips-linux-gnu-readelf")
    if tool:
        return [tool]
    if os.name == "nt":
        return ["wsl", "-d", "Ubuntu", "--", "mips-linux-gnu-readelf"]
    raise SystemExit("mips-linux-gnu-readelf is required on PATH")
