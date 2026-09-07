#!/usr/bin/env python3
"""Run one bounded native water QA fixture and retain its evidence.

Only the child process created here is stopped. This runner does not build,
change saved graphics settings, control other app windows, or install assets.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--quality", choices=["original", "modern", "high"], default="high")
    p.add_argument("--saved-quality", action="store_true", help="verify the saved UI setting instead of overriding water quality")
    p.add_argument("--style", choices=["modern", "classic"], default="modern")
    p.add_argument("--ripples", choices=["soft", "normal", "strong"], default="normal")
    p.add_argument("--spray", choices=["off", "on"], default="on")
    p.add_argument("--saved-appearance", action="store_true", help="verify saved water style and ripple settings instead of overriding them")
    p.add_argument("--fps", type=int, choices=[30, 60, 120], default=60)
    p.add_argument("--debug", type=int, choices=range(15), default=0)
    p.add_argument("--height", type=int, choices=[1080, 1440, 2160])
    p.add_argument("--msaa", type=int, choices=[1, 2, 4, 8], help="process-local sample count override")
    p.add_argument("--profiles", type=Path, help="explicit alternate course profile file for optical validation")
    p.add_argument("--compare-at", type=int, help="switch to Original at this game tick for a paused A/B capture")
    p.add_argument("--course", type=int, choices=range(9))
    p.add_argument("--players", type=int, choices=[1, 2], default=1)
    p.add_argument("--mode", choices=["trials", "championship"], default="championship")
    p.add_argument("--through", type=int, default=1500)
    p.add_argument("--timeout", type=float, default=240)
    p.add_argument("--script", type=Path, default=ROOT / "tools/scripts/water/bringup.ticks")
    p.add_argument("--app", type=Path, default=ROOT / "build-macos/WaveRace64Recomp.app", help="native app bundle, Windows build directory, or executable")
    p.add_argument("--rom", type=Path, default=ROOT / "reference/wr64-decomp/baserom.us.rev1.z64")
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--stats", action="store_true", help="enable GPU field readback; use separate runs for final timing")
    p.add_argument("--shader-failure", action="store_true", help="exercise Original fallback when water shader initialization fails")
    p.add_argument("--pass-profile", action="store_true", help="enable per-pass queries; these can perturb timing")
    p.add_argument("--motion-trace", action="store_true", help="record CPU water interpolation and vertex/UV deltas; use separate runs for final timing")
    args = p.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    # Do not accidentally inherit a different test's rendering or input setup.
    for name in list(env):
        if name.startswith(("WR64_TEST_", "WR64_WATER_", "WR64_INPUT_")) or name in ("WR64_WATER", "WR64_FRAME_PROFILE"):
            del env[name]
    env.update(WR64_WATER=args.quality, WR64_TEST_FPS=str(args.fps), WR64_TEST_SEED="12345",
               WR64_WATER_STYLE=args.style, WR64_WATER_RIPPLES=args.ripples, WR64_WATER_SPRAY=args.spray,
               WR64_WATER_DEBUG=str(args.debug),
               WR64_TEST_STOP_TICK=str(args.through + 1), WR64_INPUT_SCRIPT=str(args.script.resolve()),
               WR64_INPUT_TRACE=str(out / "input.csv"), WR64_WATER_TRACE=str(out / "state.csv"),
               WR64_FRAME_PROFILE=str(out / "frames.csv"))
    if args.saved_quality:
        env.pop("WR64_WATER")
    if args.saved_appearance:
        env.pop("WR64_WATER_STYLE")
        env.pop("WR64_WATER_RIPPLES")
        env.pop("WR64_WATER_SPRAY")
    if args.compare_at:
        env["WR64_TEST_WATER_COMPARE_TICK"] = str(args.compare_at)
    if args.height:
        env["WR64_TEST_HEIGHT"] = str(args.height)
    if args.msaa:
        env["WR64_TEST_MSAA"] = str(args.msaa)
    if args.profiles:
        env["WR64_WATER_PROFILES"] = str(args.profiles.resolve())
    if args.course is not None:
        env["WR64_TEST_COURSE"] = str(args.course)
    if args.players == 2:
        env["WR64_TEST_PLAYERS"] = "2"
    elif args.mode == "trials":
        env["WR64_TEST_MODE"] = "trials"
    if args.stats:
        env["WR64_WATER_STATS"] = str(out / "fields.csv")
    if args.shader_failure:
        env["WR64_TEST_WATER_SHADER_FAILURE"] = "1"
    if args.pass_profile:
        env["WR64_WATER_PROFILE"] = str(out / "passes.csv")
    if args.motion_trace:
        env["WR64_WATER_MOTION_TRACE"] = str(out / "motion.csv")
    app = args.app.resolve()
    if app.is_file():
        executable = app
    elif app.suffix == ".app":
        executable = app / "Contents/MacOS/WaveRace64Recomp"
    else:
        executable = app / ("WaveRace64Recomp.exe" if os.name == "nt" else "WaveRace64Recomp")
    assets = (executable.parent.parent / "Resources/assets" if executable.parent.name == "MacOS" and executable.parent.parent.name == "Contents"
              else executable.parent / "assets")
    settings = {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()}
    settings["water_overrides"] = {"quality": env.get("WR64_WATER"),
                                   "style": env.get("WR64_WATER_STYLE"),
                                   "ripples": env.get("WR64_WATER_RIPPLES"), "spray": env.get("WR64_WATER_SPRAY")}
    settings["executable"] = str(executable)
    def digest(file):
        return hashlib.sha256(file.read_bytes()).hexdigest()
    settings["executable_sha256"] = digest(executable)
    settings["script_sha256"] = digest(args.script.resolve())
    settings["profiles_sha256"] = digest(args.profiles.resolve() if args.profiles else assets / "water/profiles.json")
    (out / "settings.json").write_text(json.dumps(settings, indent=2) + "\n")
    started = time.monotonic()
    reached = False
    peak_rss_kib = None if os.name == "nt" else 0
    with (out / "runtime.log").open("w") as log:
        child = subprocess.Popen([str(executable), str(args.rom.resolve())], cwd=ROOT, env=env,
                                 stdout=log, stderr=subprocess.STDOUT)
        (out / "process.json").write_text(json.dumps({"pid": child.pid, "started_unix": time.time()}) + "\n")
        try:
            while child.poll() is None and time.monotonic() - started < args.timeout:
                if "replay reached stop tick" in (out / "runtime.log").read_text(errors="replace"):
                    reached = True
                    # Let the runtime join its workers and release the renderer.
                    # A stop marker is not proof of a clean process exit.
                    try:
                        child.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        pass
                    break
                if os.name != "nt":
                    sample = subprocess.run(["ps", "-o", "rss=", "-p", str(child.pid)], capture_output=True, text=True)
                    if sample.stdout.strip().isdigit():
                        peak_rss_kib = max(peak_rss_kib, int(sample.stdout))
                time.sleep(0.5)
        finally:
            status = child.poll()
            if status is None:
                child.terminate()
                try:
                    child.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
    # A clean shutdown can finish between the last log poll and process poll.
    reached = reached or "replay reached stop tick" in (out / "runtime.log").read_text(errors="replace")
    passed = reached and status == 0
    result = {"passed": passed, "reached_stop_tick": reached, "exit_before_cleanup": status,
              "elapsed_seconds": round(time.monotonic() - started, 2), "peak_process_rss_kib": peak_rss_kib}
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
