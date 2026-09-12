"""Finding the MIPS binutils, wherever this happens to be running.

Several scripts here read the assembled ELF with `mips-linux-gnu-readelf`. On
Windows that binary does not exist natively and the project has always reached
it through WSL, which was fine while Windows was the only host. On Linux and
macOS it is a native binary on PATH -- installed from the distribution's
`binutils-mips-linux-gnu`, or built by `tools/setup_macos.sh` -- and shelling out
to `wsl` there fails with a confusing "command not found" a long way from the
cause.

So: prefer a native one, fall back to WSL on Windows, and say plainly what is
missing anywhere else.
"""

import os
import shutil

TOOL = "mips-linux-gnu-readelf"


def readelf_command():
    """The argv prefix that runs mips-linux-gnu-readelf on this machine."""
    native = shutil.which(TOOL)
    if native:
        return [native]
    if os.name == "nt":
        return ["wsl", "-d", "Ubuntu", "--", TOOL]
    raise SystemExit(
        f"{TOOL} is not on PATH.\n"
        "Install MIPS binutils: apt install binutils-mips-linux-gnu on Debian and\n"
        "Ubuntu, or run tools/setup_macos.sh, which builds them.")
