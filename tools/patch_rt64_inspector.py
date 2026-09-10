"""The port's debug menu: a place to draw in RT64's UI, and which keys open it.

Two patches, both to RT64, both about the menu that F1 opens.

**The hook.** RT64's developer mode already answers "what did the renderer draw
here": it can pause, and right-clicking a pixel lists the draw calls under it and
highlights one. What it cannot answer is the port's half of the question -- which
of the game's elements that draw call *is*, in the identities `hud.json` uses,
and what class the display-list rewriter gave it -- and it certainly cannot
change that class while the game is paused in front of you.

Both halves have to be in one window or the tool is no better than reading two
logs side by side. RT64 owns the ImGui context, so this adds a hook it calls once
per frame from `State::inspect()`, with an ImGui frame already open, for the port
to fill in:

    extern "C" void (*RT64_PortInspectorHook)();

It is null unless the port sets it, so upstream behaviour is unchanged.
`State::inspect()` only runs in RT64's developer mode, which the port now turns
on unconditionally so that F1 works in a released build -- see
docs/HUD-INSPECTOR.md.

**The F2 shortcut.** Turning developer mode on for everyone also arms RT64's
other developer shortcuts, and one of them is not safe to leave under a finger:
F2 toggles ray tracing for the whole session, with no menu entry saying so and
nothing on screen to explain what changed. F1 (this menu), F3 (view RDRAM) and
F4 (texture replacements) are all either visibly reversible or plainly
diagnostic, so they stay. This patch removes only the F2 case from both key
filters; with no case for it, the key is not filtered and passes through to the
game like any other.

Scripted and idempotent because they patch a submodule: a submodule update would
otherwise revert them silently. Each is guarded on its own marker, so a partly
patched tree is repaired rather than doubled.

Run from the repository root:
    python tools/patch_rt64_inspector.py
"""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
STATE = REPO / "lib" / "RT64" / "src" / "hle" / "rt64_state.cpp"
APPLICATION = REPO / "lib" / "RT64" / "src" / "hle" / "rt64_application.cpp"

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

# The F2 cases, one per key filter. Removing the case leaves the key unfiltered.
F2_MARKER = "// wr64: F2 is deliberately unbound"

F2_NOTE = """            // wr64: F2 is deliberately unbound. The port turns developer
            // mode on in every build so that F1 opens the debug menu, which
            // arms these shortcuts for everyone; a session-wide ray tracing
            // toggle with nothing on screen to explain it is not something to
            // leave under a finger. F1, F3 and F4 stay.
"""

F2_CASES = (
    ("""            case VK_F2:
                processDeveloperShortcut(DeveloperShortcut::RayTracing);
                return true;
""", "windowMessageFilter"),
    ("""            case SDL_SCANCODE_F2:
                processDeveloperShortcut(DeveloperShortcut::RayTracing);
                return true;
""", "sdlEventFilter"),
)


def patch_hook():
    text = STATE.read_text(encoding="utf-8")
    if "RT64_PortInspectorHook" in text:
        print(f"  {STATE.name}: inspector hook already patched")
        return

    for anchor in (ANCHOR, DECL_ANCHOR):
        if anchor not in text:
            sys.exit(f"anchor not found in {STATE}; upstream has changed and "
                     f"this patch needs revisiting")

    text = text.replace(DECL_ANCHOR, DECL_REPLACEMENT, 1)
    text = text.replace(ANCHOR, REPLACEMENT, 1)
    STATE.write_text(text, encoding="utf-8")
    print(f"  {STATE.name}: inspector hook patched")


def patch_f2():
    text = APPLICATION.read_text(encoding="utf-8")
    if F2_MARKER in text:
        print(f"  {APPLICATION.name}: F2 already unbound")
        return

    for case, where in F2_CASES:
        if case not in text:
            sys.exit(f"the F2 case in {where} was not found in {APPLICATION}; "
                     f"upstream has changed and this patch needs revisiting")

    # The note goes in once, where the first case was; the second is just removed.
    text = text.replace(F2_CASES[0][0], F2_NOTE, 1)
    text = text.replace(F2_CASES[1][0], "", 1)
    APPLICATION.write_text(text, encoding="utf-8")
    print(f"  {APPLICATION.name}: F2 unbound")


def main():
    for target in (STATE, APPLICATION):
        if not target.exists():
            sys.exit(f"missing {target}\nRun: git submodule update --init --recursive")

    patch_hook()
    patch_f2()
    print("\nRebuild to pick it up. The debug menu is on F1.")


if __name__ == "__main__":
    main()
