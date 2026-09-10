"""Give the port a place to draw its own inspector inside RT64's UI.

RT64's developer mode already answers "what did the renderer draw here": it can
pause, and right-clicking a pixel lists the draw calls under it and highlights
one. What it cannot answer is the port's half of the question -- which of the
game's elements that draw call *is*, in the identities `hud.json` uses, and what
class the display-list rewriter gave it -- and it certainly cannot change that
class while the game is paused in front of you.

Both halves have to be in one window or the tool is no better than reading two
logs side by side. RT64 owns the ImGui context, so this adds a hook it calls once
per frame from `State::inspect()`, with an ImGui frame already open, for the port
to fill in:

    extern "C" void (*RT64_PortInspectorHook)();

It is null unless the port sets it, so upstream behaviour is unchanged, and
`State::inspect()` only runs in developer mode -- which the port turns on for
itself when WR64_INSPECTOR is set.

Scripted and idempotent because it patches a submodule: a submodule update would
otherwise revert it silently.

Run from the repository root:
    python tools/patch_rt64_inspector.py
"""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TARGET = REPO / "lib" / "RT64" / "src" / "hle" / "rt64_state.cpp"

ANCHOR = """        // Check the debugger popup regardless of the active inspector mode.
        bool debuggerSelected = debuggerInspector.checkPopup(workload);"""

REPLACEMENT = """        // Check the debugger popup regardless of the active inspector mode.
        bool debuggerSelected = debuggerInspector.checkPopup(workload);

        // The port's own inspector, drawn into this frame's UI. Null unless the
        // port sets it; see tools/patch_rt64_inspector.py.
        if (RT64_PortInspectorHook != nullptr) {
            RT64_PortInspectorHook();
        }"""

DECL_ANCHOR = """namespace RT64 {"""

DECL_REPLACEMENT = """// Set by the port to draw its own window inside RT64's inspector UI, with an
// ImGui frame already open. See tools/patch_rt64_inspector.py.
extern "C" void (*RT64_PortInspectorHook)() = nullptr;

namespace RT64 {"""


def main():
    if not TARGET.exists():
        sys.exit(f"missing {TARGET}\nRun: git submodule update --init --recursive")

    text = TARGET.read_text(encoding="utf-8")

    if "RT64_PortInspectorHook" in text:
        print(f"  {TARGET.name} already patched")
        return

    for anchor in (ANCHOR, DECL_ANCHOR):
        if anchor not in text:
            sys.exit(f"anchor not found in {TARGET}; upstream has changed and "
                     f"this patch needs revisiting")

    text = text.replace(DECL_ANCHOR, DECL_REPLACEMENT, 1)
    text = text.replace(ANCHOR, REPLACEMENT, 1)
    TARGET.write_text(text, encoding="utf-8")
    print(f"  {TARGET.name} patched")
    print("\nRebuild to pick it up, then run with WR64_INSPECTOR=1.")


if __name__ == "__main__":
    main()
