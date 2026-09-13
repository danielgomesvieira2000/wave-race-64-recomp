#!/usr/bin/env python3
"""Launch the port and record the frames its window presents, even when covered.

    python tools/capture_frames.py OUTDIR FROM TO [--scale 0.5] [--exe PATH] [--rom PATH]
                                   [--env NAME=VALUE ...]

FROM and TO are seconds after launch. Every frame the window presents in that
span is saved as OUTDIR/tMMMMMM.jpg, named by milliseconds since launch, and the
port's own output goes to OUTDIR/game.log. OUTDIR/launch.txt holds the launch
time in milliseconds since the Unix epoch, which is what the pairing log's
"wall=" field is measured against: a log frame at wall=W is the picture near
tW-launch.jpg. --env sets a variable for the port (an empty value removes it),
so a run can switch things on and off without touching the shell.

Windows only. Needs `pip install windows-capture opencv-python`.

Why not tools/capture_window.ps1: that one photographs the desktop where the
window sits, so anything covering the game -- a terminal, most often -- is what
ends up in the picture, silently. This uses Windows Graphics Capture, which
reads the window's own contents through the compositor whatever is on top, and
at a rate high enough to see a defect that lasts a few frames: about 30 to 40 a
second at half size on a laptop, which is every other frame the port presents
at 60. Frames are downscaled in the capture callback and encoded on a writer
thread, because encoding in the callback made it drop to 14 a second.

tools/contact_sheet.py tiles a run of the saved frames into one image.
"""
import argparse
import ctypes
import os
import queue
import subprocess
import sys
import threading
import time

try:
    import cv2
    from windows_capture import WindowsCapture
except ImportError:
    sys.exit("needs: pip install windows-capture opencv-python")

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TITLE = "Wave Race 64: Recompiled"


def window_of_process(pid, title):
    """The visible top-level window with this title that belongs to process pid, or 0."""
    user32 = ctypes.windll.user32
    found = []
    enum_proc = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def visit(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        owner = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value != pid:
            return True
        buffer = ctypes.create_unicode_buffer(512)
        user32.GetWindowTextW(hwnd, buffer, 512)
        if buffer.value == title:
            found.append(hwnd)
            return False
        return True

    user32.EnumWindows(enum_proc(visit), 0)
    return found[0] if found else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("outdir")
    ap.add_argument("start", type=float, help="seconds after launch to start saving")
    ap.add_argument("end", type=float, help="seconds after launch to stop and close the port")
    ap.add_argument("--scale", type=float, default=0.5)
    ap.add_argument("--exe", default=os.path.join(REPO, "build-fe", "WaveRace64Recomp.exe"))
    ap.add_argument("--rom", default=None, help="the dump; without it the launcher's remembered one is used")
    ap.add_argument("--env", action="append", default=[], metavar="NAME=VALUE")
    args = ap.parse_args()

    env = dict(os.environ)
    for item in args.env:
        name, _, value = item.partition("=")
        if value:
            env[name] = value
        else:
            env.pop(name, None)

    os.makedirs(args.outdir, exist_ok=True)
    log = open(os.path.join(args.outdir, "game.log"), "w")
    command = [args.exe] + ([args.rom] if args.rom else [])
    launched = time.perf_counter()
    with open(os.path.join(args.outdir, "launch.txt"), "w") as f:
        f.write(str(int(time.time() * 1000)))
    proc = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)

    # The window is found by the process that owns it, not by title alone: with a
    # second copy of the game already open -- the person at the machine playing --
    # a lookup by title returns whichever window the system lists first, and the
    # capture silently records the wrong game.
    hwnd = 0
    while time.perf_counter() - launched < 60 and proc.poll() is None:
        hwnd = window_of_process(proc.pid, TITLE)
        if hwnd:
            break
        time.sleep(0.2)
    if not hwnd:
        proc.kill()
        sys.exit("no window titled '%s' appeared for process %d" % (TITLE, proc.pid))

    frames = queue.Queue()
    done = threading.Event()
    counts = {"seen": 0}

    def writer():
        while True:
            item = frames.get()
            if item is None:
                return
            ms, image = item
            cv2.imwrite(os.path.join(args.outdir, "t%06d.jpg" % ms), image, [cv2.IMWRITE_JPEG_QUALITY, 88])

    writer_thread = threading.Thread(target=writer, daemon=True)
    writer_thread.start()
    capture = WindowsCapture(cursor_capture=False, draw_border=False, window_hwnd=hwnd)

    @capture.event
    def on_frame_arrived(frame, control):
        now = time.perf_counter() - launched
        if now < args.start:
            return
        if now > args.end or proc.poll() is not None:
            control.stop()
            done.set()
            return
        buffer = frame.frame_buffer
        if args.scale != 1.0:
            size = (int(frame.width * args.scale), int(frame.height * args.scale))
            image = cv2.resize(buffer, size, interpolation=cv2.INTER_LINEAR)[:, :, :3]
        else:
            image = buffer[:, :, :3].copy()
        counts["seen"] += 1
        frames.put((int(now * 1000), image))

    @capture.event
    def on_closed():
        done.set()

    control = capture.start_free_threaded()
    while not done.is_set() and proc.poll() is None and time.perf_counter() - launched < args.end + 5:
        time.sleep(0.2)
    try:
        control.stop()
    except Exception:
        pass
    frames.put(None)
    writer_thread.join()
    span = max(args.end - args.start, 0.001)
    print("%d frames saved (%.1f a second)" % (counts["seen"], counts["seen"] / span))
    if proc.poll() is None:
        proc.kill()
    log.close()


if __name__ == "__main__":
    main()
