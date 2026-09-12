"""Join the runtime's workers before releasing the memory they are reading.

**Symptom:** closing the game crashes, intermittently, after everything has
been reported shut down -- a different crash from the SDL event filter one that
`tools/patch_rt64_eventfilter.py` fixes, and it survives that fix.

ultramodern's game and timer threads are asked to stop and then not waited for.
The shutdown path goes on to release the thread queues and RDRAM while those
threads may still be inside a wait on one of them, or still dereferencing
`rdram`. Whether it crashes depends on where each thread happened to be, which
is why it is intermittent and why it showed up under repeated scripted runs long
before anyone hit it by hand.

The fix is the missing half of the pairing: wake the workers, join them, and
only then let go of what they were using.

Like `tools/patch_rt64_water.py` this is a diff rather than anchored strings --
it spans five files across librecomp and ultramodern -- and it is idempotent the
same way: `git apply --reverse --check` succeeds only when it is already fully
applied.

Scripted because it patches a submodule: a submodule update would otherwise
revert it silently.

Run from the repository root:
    python tools/patch_runtime_shutdown.py
"""

from pathlib import Path

from patch_rt64_water import apply_patch

REPO = Path(__file__).resolve().parent.parent
SUBMODULE = REPO / "lib" / "N64ModernRuntime"
PATCH = REPO / "tools" / "patches" / "runtime-shutdown.patch"
NAME = "runtime shutdown"


def main() -> int:
    return apply_patch(SUBMODULE, PATCH, NAME)


if __name__ == "__main__":
    raise SystemExit(main())
