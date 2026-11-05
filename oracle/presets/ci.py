#!/usr/bin/env python3
"""The oracle job's logic, so that what the workflow decides is tested (oracle/presets/README.md).

    ci.py gate                       reads the changed paths on standard input; prints yes or no
    ci.py run MISSION INPUT --build-root DIR --traces DIR [--env-file FILE]
    ci.py compare MISSION INPUT --traces DIR --ran "PRESET PRESET" [--expected DIR]

`run` runs run_preset.sh for each preset, in order, one at a time. Its exit code means: 0 the preset
ran (kept for the comparison), 3 it skipped (a ::warning:: names the preset and the reason), anything
else fails the step after the other presets have run. It appends `RAN_<MISSION>=<presets>` to
--env-file. `compare` runs compare_presets.py on the presets that ran, with --expected where the
folder holds a recorded trace for the mission, and warns when fewer than two presets ran so that no
pairwise comparison happened. Exit status: that of compare_presets.py (0 identical, 4 a tick
differs, 2 a trace is bad), or 1 when a preset failed.
"""
import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
PRESETS = ('macos-arm64', 'linux-x64', 'windows-x64')
# What the oracle judges: the game's source, the native platform layer, and the oracle's own
# runner, driver, trace tools and this workflow.
GATE = re.compile(r'^(game/|src/|oracle/presets/|oracle/driver/|oracle/trace/|'
                  r'\.github/workflows/pull-request\.yml$)')


def path_gate(files):
    """True when any changed path is one the oracle judges."""
    return any(GATE.match(f.strip()) for f in files)


def classify(returncode):
    """The meaning of run_preset.sh's exit status for the workflow."""
    if returncode == 0:
        return 'ran'
    if returncode == 3:
        return 'skip'
    return 'fail'


def run_preset_script():
    return os.environ.get('SFCOP_ORACLE_RUN_PRESET') or str(HERE / 'run_preset.sh')


def run_all(mission, input_name, build_root, traces, presets=PRESETS):
    """Run each preset in turn. Returns (ran, failed, lines for the log)."""
    ran, failed, log = [], False, []
    for preset in presets:
        # A scratch copy left by a killed run is removed first (run_preset.sh does the same).
        shutil.rmtree(pathlib.Path(build_root) / preset / 'oracle-scratch', ignore_errors=True)
        proc = subprocess.run(
            ['sh', run_preset_script(), preset, '--build', str(pathlib.Path(build_root) / preset),
             '--root', str(traces), '--mission', mission, '--input', input_name],
            capture_output=True, text=True)
        out = proc.stdout.strip()
        kind = classify(proc.returncode)
        log.append(f'oracle: {preset}: {mission}/{input_name}: exit {proc.returncode} ({kind})')
        if proc.stdout:
            log.append(proc.stdout.rstrip('\n'))
        if proc.stderr:
            log.append(proc.stderr.rstrip('\n'))
        if kind == 'ran':
            ran.append(preset)
        elif kind == 'skip':
            reason = out.splitlines()[-1] if out else f'skip: {preset}'
            log.append(f'::warning::oracle {mission}/{input_name}: {preset} did not run: '
                       f'{reason.removeprefix("skip: ")}')
        else:
            failed = True
            log.append(f'::error::oracle {mission}/{input_name}: {preset} failed (exit {proc.returncode})')
    return ran, failed, log


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = p.add_subparsers(dest='cmd', required=True)
    sub.add_parser('gate')
    r = sub.add_parser('run')
    r.add_argument('mission')
    r.add_argument('input')
    r.add_argument('--build-root', required=True)
    r.add_argument('--traces', required=True)
    r.add_argument('--env-file')
    c = sub.add_parser('compare')
    c.add_argument('mission')
    c.add_argument('input')
    c.add_argument('--traces', required=True)
    c.add_argument('--ran', default='')
    c.add_argument('--expected')
    args = p.parse_args(argv)

    if args.cmd == 'gate':
        print('yes' if path_gate(sys.stdin.read().splitlines()) else 'no')
        return 0

    if args.cmd == 'run':
        ran, failed, log = run_all(args.mission, args.input, args.build_root, args.traces)
        print('\n'.join(log))
        if args.env_file:
            with open(args.env_file, 'a') as f:
                f.write(f'RAN_{args.mission.upper()}={" ".join(ran)}\n')
        return 1 if failed else 0

    ran = args.ran.split()
    if not ran:
        print(f'::warning::oracle {args.mission}/{args.input}: no preset ran, so nothing was compared')
        return 0
    if len(ran) < 2:
        print(f'::warning::oracle {args.mission}/{args.input}: only {ran[0]} ran, so no pairwise '
              'comparison happened')
    cmd = [sys.executable, '-I', str(HERE / 'compare_presets.py'), args.traces, *ran,
           '--mission', args.mission, '--input', args.input]
    if args.expected:
        cmd += ['--expected', args.expected]
    return subprocess.run(cmd).returncode


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
