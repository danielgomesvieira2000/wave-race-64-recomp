#!/usr/bin/env python3
"""Capture RT64 source textures across explicit native game fixtures.

This records observed textures, not proof of complete authored-asset coverage.
Each child has its own dump and logs; other running game processes are untouched.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--app', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--courses', type=int, nargs='+', default=list(range(9)), choices=range(9))
    p.add_argument('--players', type=int, nargs='+', default=[1, 2], choices=[1, 2])
    p.add_argument('--jobs', type=int, default=1, choices=[1, 2])
    p.add_argument('--through', type=int, default=1600)
    p.add_argument('--timeout', type=int, default=220)
    args = p.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    jobs = [(course, players) for players in args.players for course in args.courses]

    def run(job):
        course, players = job
        name = f'course-{course}-{players}p'
        target = output / name
        result_path = target / 'run/result.json'
        if result_path.exists():
            old = json.loads(result_path.read_text())
            if old.get('passed'):
                return name, old
            raise RuntimeError(f'{name} previously failed; retain evidence and choose a new output directory')
        target.mkdir(exist_ok=True)
        env = os.environ.copy()
        env.pop('WR64_TEXTURE_PACK', None)
        env.update(SDL_AUDIODRIVER='dummy', WR64_TEXTURE_DUMP=str(target / 'dump'))
        command = [sys.executable, str(ROOT / 'tools/run_water_replay.py'),
                   '--app', str(args.app.resolve()), '--output', str(target / 'run'),
                   '--quality', 'original', '--fps', '30', '--course', str(course),
                   '--mode', 'trials', '--players', str(players), '--through', str(args.through),
                   '--timeout', str(args.timeout)]
        print(f'Starting {name}', flush=True)
        with (target / 'runner.log').open('w') as log:
            process = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        result = json.loads(result_path.read_text()) if result_path.exists() else {'passed': False}
        result.update(returncode=process.returncode, dump_pairs=len(list((target / 'dump').glob('*.tile.json'))))
        print(f'Finished {name}: {result}', flush=True)
        return name, result

    results = {}
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(run, job) for job in jobs]
        for future in as_completed(futures):
            name, result = future.result()
            results[name] = result
            (output / 'matrix.json').write_text(json.dumps({
                'coverage': 'observed native fixtures; menus, rider variants, animations and culled geometry require additional coverage',
                'through_tick': args.through, 'fixtures': results}, indent=2) + '\n')
    return 0 if all(r.get('passed') for r in results.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())
