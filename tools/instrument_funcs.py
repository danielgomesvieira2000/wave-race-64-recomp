"""Phase 04 diagnostic: trace when specific recompiled functions run.

Boot bring-up questions are almost always about order and reachability -- did
this initialiser run before that user of what it initialises? The recompiled
code answers nothing by itself: it is 20 MB of generated C with no logging, and
N64Recomp's trace_mode logs every function, which is far too much to read.

This inserts a one-shot printf at the entry of named functions only. It edits
generated code, which is normally forbidden, and that is safe here for one
reason: the edits are erased the moment the recompiler runs again, so nothing
can silently persist into a real build. Re-run tools/wsl_recompile.sh to remove
them.

Usage, from the repository root:
    python tools/instrument_funcs.py SysMain_GfxInitBuffers SysMain_GfxFullSync
    python tools/instrument_funcs.py --clear
"""

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GENERATED = REPO / "RecompiledFuncs"
MARKER = "/* wr64-trace */"


def clear():
    removed = 0
    for path in sorted(GENERATED.glob("funcs_*.c")):
        text = path.read_text(errors="replace")
        if MARKER not in text:
            continue
        cleaned = re.sub(r"^.*" + re.escape(MARKER) + r".*\n", "", text, flags=re.M)
        path.write_text(cleaned)
        removed += text.count(MARKER)
    print(f"removed {removed} trace line(s)")


def instrument(names):
    # "NAME" traces entry once; "NAME@0xADDR" traces every entry and prints the
    # RDRAM word at ADDR, which is how a value that starts valid and later goes
    # bad gets caught.
    watches = {}
    wanted = set()
    for entry in names:
        if "@" in entry:
            name, addr = entry.split("@", 1)
            watches[name] = addr.lower().removeprefix("0x")
            wanted.add(name)
        else:
            wanted.add(entry)

    done = set()

    for path in sorted(GENERATED.glob("funcs_*.c")):
        text = path.read_text(errors="replace")
        original = text

        for name in sorted(wanted - done):
            # Generated definitions look like:
            #   RECOMP_FUNC void NAME(uint8_t* rdram, recomp_context* ctx) {
            pattern = re.compile(
                r"(RECOMP_FUNC void " + re.escape(name) +
                r"\(uint8_t\* rdram, recomp_context\* ctx\) \{\n)")
            match = pattern.search(text)
            if not match:
                continue

            # NAME@0xADDR additionally prints the RDRAM word at that KSEG0
            # address on every entry, and reports the call count. Order alone
            # answers "did it run"; a watched value answers "and did it stay
            # valid", which is the question once an initialiser is known to have
            # run before its user.
            watch = watches.get(name)
            if watch is None:
                trace = (
                    '    { static int seen = 0; if (!seen++) { '
                    'fprintf(stderr, "[wr64-trace] ' + name + '\\n"); fflush(stderr); } } '
                    + MARKER + "\n")
            else:
                trace = (
                    '    { static int calls = 0; ++calls; '
                    'fprintf(stderr, "[wr64-trace] ' + name + ' #%d: [0x' + watch +
                    '] = 0x%08X\\n", calls, '
                    '*(uint32_t*)(rdram + (0x' + watch + 'u & 0x00FFFFFFu))); '
                    'fflush(stderr); } ' + MARKER + "\n")
            text = text[:match.end()] + trace + text[match.end():]
            done.add(name)

        if text != original:
            # The generated files do not include stdio.
            if "#include <stdio.h>" not in text:
                text = "#include <stdio.h>\n" + text
            path.write_text(text)

    for name in sorted(wanted):
        print(f"  {name:<30} {'instrumented' if name in done else 'NOT FOUND'}")

    missing = wanted - done
    if missing:
        print("\nSome functions were not found. They may be stubbed, ignored, or "
              "named differently in the generated code.")


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    if args[0] == "--clear":
        clear()
        return
    instrument(args)


if __name__ == "__main__":
    main()
