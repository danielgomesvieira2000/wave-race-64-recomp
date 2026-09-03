"""Make librecomp's function-lookup failures diagnosable.

A failed lookup prints only the address it could not find:

    Failed to find function at 0x802C5800

and then asserts and exits. That is a dead end for phase 04. The address alone
does not say which function asked for it, and with 49 threads running it does
not say which thread either -- and this project has already been misled once by
reasoning about "the next iteration" of a loop that several threads share.

An exit is not an exception, so the crash handler never sees it. This patches
the miss path to call a hook the project implements, which reports the calling
address and thread, resolves the caller to a function name and source line, and
then lets the original assert and exit proceed unchanged.

Scripted and idempotent because it patches a submodule: a submodule update would
otherwise revert it silently.

Run from the repository root:
    python tools/patch_librecomp.py
"""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TARGET = REPO / "lib" / "N64ModernRuntime" / "librecomp" / "src" / "overlays.cpp"

ANCHOR = """extern "C" recomp_func_t * get_function(int32_t addr) {
    auto func_find = func_map.find(addr);
    if (func_find == func_map.end()) {
        fprintf(stderr, "Failed to find function at 0x%08X\\n", addr);"""

REPLACEMENT = """// Implemented by the project; reports the caller and thread of a failed lookup.
extern "C" void wr64_report_lookup_miss(unsigned int addr, void* return_address);

extern "C" recomp_func_t * get_function(int32_t addr) {
    auto func_find = func_map.find(addr);
    if (func_find == func_map.end()) {
        wr64_report_lookup_miss((unsigned int)addr, __builtin_return_address(0));
        fprintf(stderr, "Failed to find function at 0x%08X\\n", addr);"""


def main():
    if not TARGET.exists():
        sys.exit(f"missing {TARGET}\nRun: git submodule update --init --recursive")

    text = TARGET.read_text()

    if "wr64_report_lookup_miss" in text:
        print(f"  {TARGET.name} already patched")
        return

    if ANCHOR not in text:
        sys.exit(f"anchor not found in {TARGET}; upstream has changed and this "
                 f"patch needs revisiting")

    TARGET.write_text(text.replace(ANCHOR, REPLACEMENT, 1))
    print(f"  {TARGET.name} patched")
    print("\nRebuild to pick it up.")


if __name__ == "__main__":
    main()
