#!/usr/bin/env python3
"""Capture rider banks and additional native scenes with explicit coverage evidence.

Use --plan to review without launching. These fixtures complement capture_matrix.py;
neither a bank-selection count nor a hash plateau proves all authored textures drew.
Every native process has its own dump and logs; saved settings and other running
processes are untouched. Process-only rider overrides require texture_capture.cpp.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import csv
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def plan_fixtures(groups, courses, through):
    fixtures = []
    if 'riders' in groups:
        # Native 2P asset setup loads both primary riders and both secondary
        # model variants. Two distinct pairs times two outfits covers 16 banks.
        for alternate in (0, 1):
            for rider in (0, 2):
                fixtures.append(dict(name=f'riders-{rider}-{rider + 1}-alt{alternate}',
                    course=1, players=2, mode='versus', rider=rider,
                    alternate=alternate, difficulty=0, through=through))
    if 'course-modes' in groups:
        for course in courses:
            for mode, difficulty in [('championship', 0), ('championship', 1),
                                     ('championship', 2), ('stunt', 2)]:
                fixtures.append(dict(name=f'course-{course}-{mode}-difficulty{difficulty}',
                    course=course, players=1, mode=mode, rider=course % 4,
                    alternate=course % 2, difficulty=difficulty, through=through))
    if 'attract' in groups:
        fixtures.append(dict(name='attract-no-input', players=1,
                             through=max(through, 9000), no_input=True))
    if 'menus' in groups:
        for scene, state in [('options', 0x3c), ('names', 0x3e), ('records', 0x42),
                             ('conditions', 0x44), ('audio', 0x48), ('erase-page', 0x46),
                             ('save-load-page', 0x40)]:
            fixtures.append(dict(name=f'menu-{scene}', scene=scene, expected_state=state,
                                 players=1, through=min(through, 1500)))
    if 'postrace' in groups:
        for scene in ('results', 'finish-hud'):
            fixtures.append(dict(name=f'postrace-{scene}', postrace=scene,
                                 expected_state=0x34 if scene == 'results' else 0x28,
                                 required_state=0x34 if scene == 'results' else 0x2a,
                                 course=1, players=1, mode='championship', rider=0,
                                 alternate=0, difficulty=0, power=5, through=max(through, 2100)))
        fixtures.append(dict(name='postrace-finish-fourth', postrace='finish-hud',
                             expected_state=0x28, required_state=0x2a, course=1,
                             players=1, mode='championship', rider=0, alternate=0,
                             difficulty=0, power=5, misses=2, finish_place=3, finish_delay=360,
                             through=max(through, 2100)))
    if 'overviews' in groups:
        # Expert's eight-entry round table includes all eight race courses.
        # Dolphin Park's tutorial overview is already in each race capture.
        for round_index in range(8):
            fixtures.append(dict(name=f'overview-round-{round_index}', scene='overview',
                                 round=round_index, difficulty=2, expected_state=0x1e,
                                 players=1, through=min(through, 1050)))
    return fixtures


def dump_hashes(directory):
    return {p.name.removesuffix('.v5.tile.json')
            for p in directory.rglob('*.v5.tile.json')}


def observe_run(target, fixture, inventory):
    result_path = target / 'run/result.json'
    result = json.loads(result_path.read_text()) if result_path.exists() else {'passed': False}
    log_path = target / 'run/runtime.log'
    log = log_path.read_text(errors='replace') if log_path.exists() else ''
    races = [dict(zip(('course', 'mode', 'players', 'difficulty'), map(int, values)))
             for values in re.findall(
                 r'\[texture-capture\] race course=(\d+) mode=(\d+) players=(\d+) difficulty=(\d+)', log)]
    starts = sorted({int(value, 16) for value in re.findall(
        r'\[texture-capture\] rider-bank slot=\d+ rom_start=([0-9a-f]+)', log)})
    bank_ids = {b['rom_start']: b['id'] for b in inventory.get('banks', [])}
    selected_banks = sorted({bank_ids[start] for start in starts if start in bank_ids})
    variants = [v['table_index'] for v in inventory.get('rider_bank_variants', [])
                if v['bank'] in selected_banks]
    states = []
    state_path = target / 'run/state.csv'
    state_rows = []
    if state_path.exists():
        with state_path.open() as state_file:
            state_rows = list(csv.DictReader(state_file))
        for row in state_rows:
            state = int(row['state'])
            if state not in states:
                states.append(state)
    initialized = bool(races) or fixture.get('no_input', False)
    result.update(capture_fixture_initialized=initialized, observed_races=races,
                  selected_rider_bank_rom_starts=[f'{v:08x}' for v in starts],
                  selected_rider_banks=selected_banks, selected_rider_variant_indices=variants,
                  observed_game_states=states, source_hash_count=len(dump_hashes(target / 'dump')))
    # A clean, bounded process exit without reaching the intended native race
    # initializer is not a successful race-capture fixture.
    if fixture.get('scene'):
        expected = fixture['expected_state']
        matched = bool(re.search(rf'\[texture-capture\] scene={re.escape(fixture["scene"])} expected_state={expected} actual_state={expected}', log))
        matched = matched and expected in states
        # Verify the exclusive pad has no input after the first observed page
        # transition. Merely using a nominally idle script is not proof.
        first_page_tick = None
        for row in state_rows:
            if int(row['state']) == expected:
                first_page_tick = int(row['tick'])
                break
        input_path = target / 'run/input.csv'
        idle = first_page_tick is not None and input_path.exists()
        if idle:
            with input_path.open() as input_file:
                for row in csv.DictReader(input_file):
                    if int(row['tick']) >= first_page_tick and (int(row['buttons']) or float(row['x']) or float(row['y'])):
                        idle = False
                        break
        result.update(capture_fixture_initialized=matched, capture_fixture_matched=matched,
                      no_input_on_menu_page=idle)
        result['passed'] = bool(result.get('passed') and matched and idle)
    elif fixture.get('postrace'):
        entry = re.search(rf'\[texture-capture\] postrace={re.escape(fixture["postrace"])} tick=(\d+) actual_state={fixture["expected_state"]} finish_flags=1,1,1,1 retire_flags=0,0,0,0', log)
        matched = entry is not None
        matched = matched and fixture.get('required_state', fixture['expected_state']) in states
        input_path = target / 'run/input.csv'
        idle = entry is not None and input_path.exists()
        if idle:
            with input_path.open() as input_file:
                idle = all(not (int(row['buttons']) or float(row['x']) or float(row['y']))
                           for row in csv.DictReader(input_file) if int(row['tick']) >= int(entry[1]))
        result.update(capture_fixture_initialized=matched, capture_fixture_matched=matched,
                      no_input_after_finish=idle,
                      fixture_method='Native scene initializer with synthetic successful completion flags and times')
        result['passed'] = bool(result.get('passed') and matched and idle)
    elif not fixture.get('no_input'):
        mode = {'trials': 0, 'versus': 1, 'championship': 4, 'stunt': 11}[fixture['mode']]
        matches = any(r['course'] == fixture['course'] and r['mode'] == mode
                      and r['players'] == fixture['players'] for r in races)
        result['capture_fixture_matched'] = matches
        result['passed'] = bool(result.get('passed') and matches)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--groups', nargs='+', choices=['riders', 'course-modes', 'attract', 'menus', 'postrace', 'overviews'], default=['riders'])
    parser.add_argument('--only', action='append', default=[], help='capture only these exact planned fixture names; repeatable')
    parser.add_argument('--courses', type=int, nargs='+', choices=range(9), default=list(range(9)))
    parser.add_argument('--through', type=int, default=2400)
    parser.add_argument('--timeout', type=int, default=360)
    parser.add_argument('--jobs', type=int, choices=[1, 2], default=1)
    parser.add_argument('--inventory', type=Path, default=ROOT / 'build/texture-qa/rom-inventory.json')
    parser.add_argument('--baseline', type=Path, action='append', default=[])
    parser.add_argument('--plan', action='store_true')
    args = parser.parse_args()
    fixtures = plan_fixtures(args.groups, args.courses, args.through)
    if args.only:
        missing = set(args.only) - {fixture['name'] for fixture in fixtures}
        if missing:
            parser.error(f'unknown fixture names for selected groups: {sorted(missing)}')
        fixtures = [fixture for fixture in fixtures if fixture['name'] in args.only]
    if args.plan:
        print(json.dumps(fixtures, indent=2))
        return 0
    if args.app is None:
        parser.error('--app is required unless --plan is used')
    if any(group in args.groups for group in ('postrace', 'overviews')):
        portable = args.app.resolve() / 'Contents/MacOS/portable.txt'
        if not portable.exists():
            parser.error('postrace/overview captures require a separate portable bundle to isolate native record writes')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    inventory = json.loads(args.inventory.read_text())
    baseline = set().union(*(dump_hashes(path.resolve()) for path in args.baseline))
    (output / 'plan.json').write_text(json.dumps(fixtures, indent=2) + '\n')

    def run(fixture):
        target = output / fixture['name']
        request = dict(fixture, executable=str(args.app.resolve()))
        request_path = target / 'fixture.json'
        if request_path.exists():
            if json.loads(request_path.read_text()) != request:
                raise RuntimeError(f'{target}: previous fixture differs; choose a fresh output directory')
            result = observe_run(target, fixture, inventory)
            if result.get('passed'):
                return fixture['name'], result
            raise RuntimeError(f'{target}: previous run incomplete/failed; preserve evidence and choose a fresh output directory')
        target.mkdir(exist_ok=False)
        request_path.write_text(json.dumps(request, indent=2) + '\n')
        env = {key: value for key, value in os.environ.items()
               if not key.startswith('WR64_TEXTURE_CAPTURE_') and key != 'WR64_TEXTURE_PACK'}
        env.update(SDL_AUDIODRIVER='dummy', WR64_TEXTURE_DUMP=str(target / 'dump'))
        for key in ('rider', 'alternate', 'difficulty', 'mode', 'scene', 'round', 'postrace', 'power', 'misses', 'finish_place', 'finish_delay'):
            if key in fixture:
                env[f'WR64_TEXTURE_CAPTURE_{key.upper()}'] = str(fixture[key])
        command = [sys.executable, str(ROOT / 'tools/run_water_replay.py'),
                   '--app', str(args.app.resolve()), '--output', str(target / 'run'),
                   '--quality', 'original', '--fps', '30', '--players', str(fixture['players']),
                   '--through', str(fixture['through']), '--timeout', str(args.timeout)]
        if 'course' in fixture:
            command += ['--course', str(fixture['course'])]
        if fixture.get('no_input'):
            script = target / 'no-input.ticks'
            script.write_text(f'@ticks\n0 {fixture["through"] + 2} - # Exclusive idle title and attract flow.\n')
            command += ['--script', str(script)]
        elif fixture.get('scene'):
            script = target / 'menu-entry.ticks'
            script.write_text('@ticks\n90 100 START\n200 210 A\n320 330 A\n440 450 A\n560 561 A\n'
                              f'562 {fixture["through"] + 2} - # No input once the native page initializes.\n')
            command += ['--script', str(script)]
        elif fixture.get('postrace'):
            script = target / 'postrace-entry.ticks'
            release = 1430 if fixture.get('finish_delay', 180) > 180 else 1250
            script.write_text('@ticks\n90 100 START\n200 210 A\n320 330 A\n440 450 A\n560 570 A\n'
                              f'680 690 A\n800 810 A\n950 {release} A\n'
                              f'{release + 1} {fixture["through"] + 2} - # No input on finish/results screens.\n')
            command += ['--script', str(script)]
        print(f"Starting {fixture['name']}", flush=True)
        with (target / 'runner.log').open('w') as log:
            process = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        result = observe_run(target, fixture, inventory)
        result['runner_returncode'] = process.returncode
        print(f"Finished {fixture['name']}: passed={result['passed']}, hashes={result['source_hash_count']}", flush=True)
        return fixture['name'], result

    results = {}
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for future in as_completed([pool.submit(run, fixture) for fixture in fixtures]):
            name, result = future.result()
            results[name] = result
            hashes = dump_hashes(output)
            variants = sorted({index for r in results.values() for index in r['selected_rider_variant_indices']})
            report = dict(coverage_kind='observed native fixtures and native rider DMA selection',
                limitations=['Selected banks can contain textures that were not rendered.',
                             'Unvisited course sectors, results, options, animals and animation frames remain separate coverage requirements.'],
                fixtures=results, selected_rider_variants=variants,
                expected_rider_variant_count=len(inventory['rider_bank_variants']),
                source_hash_count=len(hashes), new_source_hash_count=len(hashes - baseline),
                new_source_hashes=sorted(hashes - baseline))
            (output / 'coverage.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0 if all(result.get('passed') for result in results.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())
