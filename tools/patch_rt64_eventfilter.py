"""Make RT64 take its SDL event filter back off when it shuts down.

**Symptom:** the process dies with an access violation every time the game is
closed normally, after everything has been reported shut down, at an address in
no loaded module::

    [wr64] recomp::start returned -- runtime shut down
    [wr64] shutting down: controller
    [wr64] shutting down: window
    [wr64] ==== CRASH ====
    [wr64] ACCESS_VIOLATION (0xC0000005) at 0000019704441630
    [wr64] while executing address 0x19704441630

`ApplicationWindow::setup` chains itself into SDL's event filter, keeping the
previous one to forward to::

    SDL_GetEventFilter(&sdlEventFilterStored, &sdlEventFilterUserdata);
    SDL_SetEventFilter(&ApplicationWindow::sdlEventFilter, this);
    sdlEventFilterInstalled = true;

`~ApplicationWindow` clears `HookedApplicationWindow` and unhooks the Win32
message hook, but never removes that filter. So after RT64 has gone, SDL still
holds a pointer to the destroyed `ApplicationWindow`, and the first event to
arrive afterwards runs::

    ApplicationWindow *appWindow = reinterpret_cast<ApplicationWindow *>(userdata);
    if (appWindow->listener->sdlEventFilter(event)) {

`listener` is read out of freed memory and the call through it is virtual, so
the jump lands wherever the dead object's vtable pointer now points. That is the
address in no module.

The event that triggers it is unavoidable: `SDL_DestroyWindow` pumps the window's
messages, which is why the crash is at shutdown and is perfectly reproducible.
The stack could not be unwound through the faulting frame -- freed memory has no
unwind data -- and was recovered by scanning the stack for return addresses,
which named `RT64::ApplicationWindow::sdlEventFilter + 0x16` directly.

The fix is the half of the pairing that is missing: put back the filter that was
there before, in the destructor, exactly as the Win32 hook beside it is undone.
Restoring the stored filter rather than clearing it matters, because RT64 chained
onto whatever was already installed and clearing would drop that too.

Scripted and idempotent because it patches a submodule: a submodule update would
otherwise revert it silently.

Run from the repository root:
    python tools/patch_rt64_eventfilter.py
"""

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TARGET = REPO / "lib" / "RT64" / "src" / "hle" / "rt64_application_window.cpp"

MARKER = "wr64: the SDL event filter has to come back off"

ANCHOR = """    ApplicationWindow::~ApplicationWindow() {
        if (HookedApplicationWindow == this) {
            HookedApplicationWindow = nullptr;
        }
"""

REPLACEMENT = """    ApplicationWindow::~ApplicationWindow() {
        if (HookedApplicationWindow == this) {
            HookedApplicationWindow = nullptr;
        }

        // Added by the Wave Race 64 port -- %s.
        // See tools/patch_rt64_eventfilter.py.
        //
        // setup() chained this object into SDL's event filter and nothing took
        // it out again, so SDL went on holding a pointer to it after it was
        // destroyed. The next event -- and SDL_DestroyWindow pumps messages, so
        // there is always a next event -- read `listener` out of freed memory
        // and made a virtual call through it. Put back whatever filter was
        // installed before this one, rather than clearing: this one was chained
        // onto it and clearing would drop it too.
        if (sdlEventFilterInstalled) {
            SDL_SetEventFilter(sdlEventFilterStored, sdlEventFilterUserdata);
            sdlEventFilterInstalled = false;
        }
""" % MARKER


def main():
    if not TARGET.exists():
        sys.exit(f"missing {TARGET}\nRun: git submodule update --init --recursive")

    text = TARGET.read_text(encoding="utf-8")

    if MARKER in text:
        print(f"  {TARGET.name} already patched")
        return

    if ANCHOR not in text:
        sys.exit(f"anchor not found in {TARGET}; upstream has changed and this "
                 f"patch needs revisiting")

    TARGET.write_text(text.replace(ANCHOR, REPLACEMENT, 1), encoding="utf-8")
    print(f"  {TARGET.name} patched")


if __name__ == "__main__":
    main()
