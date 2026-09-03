"""Expose N64Recomp's use_lookup_for_all_function_calls as a config option.

Wave Race 64 calls into its overlays with direct `jal`s from resident code. That
cannot be resolved statically, and the tool is right about it: resolve_jal never
treats a function in a relocatable section as a candidate from another section,
because which overlay is resident is a runtime fact. Nineteen sections share
vram 0x802C5800 here, so the address alone cannot say which function is meant.

N64Recomp already solves this. `Context::use_lookup_for_all_function_calls`
makes every call go through the runtime's function lookup, which resolves an
address against whatever is loaded -- exactly the semantics an overlay needs.
The field exists and the code path is in resolve_jal. It simply has no config
key, in our pinned revision or upstream, so the CLI can never set it.

This adds the key. It is three insertions following the pattern `trace_mode`
already establishes, and it is applied to a submodule, so it is scripted and
idempotent rather than hand-edited: a submodule update would otherwise silently
revert it and the port would stop building with no obvious reason.

The alternative was hand-transcribing func_80092CF0 -- a 302-instruction
dispatcher with a 104-entry jump table over game states -- and its two
companions. That is far more code, and far more to get subtly wrong.

Run from the repository root, after cloning submodules:
    python tools/patch_n64recomp.py
"""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "lib" / "N64ModernRuntime" / "N64Recomp" / "src"

OPTION = "use_lookup_for_all_function_calls"

EDITS = [
    (
        SRC / "config.h",
        "        bool trace_mode;",
        "        bool trace_mode;\n"
        "        // Route every function call through the runtime lookup. Needed for\n"
        "        // games whose resident code calls directly into overlays.\n"
        f"        bool {OPTION};",
    ),
    (
        SRC / "config.cpp",
        '        std::optional<bool> trace_mode_opt = input_data["trace_mode"].value<bool>();',
        f'        std::optional<bool> lookup_all_opt = input_data["{OPTION}"].value<bool>();\n'
        f"        {OPTION} = lookup_all_opt.value_or(false);\n"
        "\n"
        '        std::optional<bool> trace_mode_opt = input_data["trace_mode"].value<bool>();',
    ),
    (
        SRC / "main.cpp",
        "    // Propogate the trace mode parameter.\n"
        "    context.trace_mode = config.trace_mode;",
        "    // Propogate the trace mode parameter.\n"
        "    context.trace_mode = config.trace_mode;\n"
        "\n"
        "    // Route all calls through the runtime lookup when asked to.\n"
        f"    context.{OPTION} = config.{OPTION};",
    ),
]


def main():
    if not SRC.exists():
        sys.exit(f"missing {SRC}\nRun: git submodule update --init --recursive")

    applied = 0
    already = 0

    for path, anchor, replacement in EDITS:
        text = path.read_text()
        if replacement.split("\n")[0] in text and OPTION in text:
            already += 1
            print(f"  {path.name:<12} already patched")
            continue
        if anchor not in text:
            sys.exit(f"anchor not found in {path}; upstream has changed and this "
                     f"patch needs revisiting:\n  {anchor.splitlines()[0]}")
        path.write_text(text.replace(anchor, replacement, 1))
        applied += 1
        print(f"  {path.name:<12} patched")

    print(f"\n{applied} file(s) patched, {already} already up to date")
    if applied:
        print("Rebuild the recompiler: wsl -d Ubuntu -- bash tools/wsl_build_recompiler.sh")


if __name__ == "__main__":
    main()
