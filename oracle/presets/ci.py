#!/usr/bin/env python3
"""The oracle job's logic, so that what the workflow decides is tested (oracle/presets/README.md).

    ci.py gate                       reads the changed paths on standard input; prints yes or no
    ci.py builds PRESET              reads the changed paths on standard input; prints yes or no:
                                     whether the build job builds that preset for this change, and
                                     for no, a ::notice:: with the reason on standard error
    ci.py run MISSION INPUT --build-root DIR --traces DIR [--env-file FILE]
    ci.py compare MISSION INPUT --traces DIR --ran "PRESET PRESET" [--expected DIR]

`run` runs run_preset.sh for each preset, in order, one at a time. Its exit code means: 0 the preset
ran (kept for the comparison), 3 it skipped (a ::warning:: names the preset and the reason), anything
else fails the step after the other presets have run. A paused preset (PAUSED_PRESETS) is not run. It appends `RAN_<MISSION>=<presets>` to
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


# A change made only of decompiled source and its records (a grind pull request) builds and
# judges macOS alone: the cross presets compile the same game/ code with the same lifter, and the
# platform layer they exercise did not change. The same holds for the lift's own inputs outside
# game/ (cmake/lifter/objlift.cmake lists them): the harness library, the unit profiles, the
# analysis exports and tables, and the object lift. harness/lib/paths.py is not among them: every
# preset's configure reads it (cmake/lifter/locate.py). analysis/results/*.tsv is the lift's own
# glob; a .tsv put there that is not a lift table belongs elsewhere. game/port/ (the platform-forced shims) and
# game/UNITS.tsv are not decompiled source, and of analysis/results/ only the harness's records
# and the lift's tables are.
DECOMP_ONLY = re.compile(r'^(game/(?!port/|UNITS\.tsv$)|analysis/results/(matched_set\.json|difficult_functions\.tsv)$|'
                         r'src/app/guest/override\.cpp$|'
                         r'harness/lib/(?!paths\.py$)|harness/units\.json$|ghidra/exports/|analysis/results/[^/]+\.tsv$|'
                         r'tools/objlift/)')
DECOMP_PRESET = 'macos-arm64'

# What no build reads: documentation, the Python tests and the harness's own tools outside the
# library the lift uses, the analysis scripts and notes, the Ghidra pipeline outside its exports,
# and the check and progress scripts. A change made only of these builds no preset; the check
# job still runs the tests. cmake/, CMakeLists.txt and the sources the build globs are never here,
# nor tests/fixtures/, which the C++ tests read. This is a deny-list: a path that a build step or a
# compiled test starts reading must be taken out of it in the same change.
NO_BUILD = re.compile(r'^(docs/|tests/(?!fixtures/)|harness/(tests|hooks|prompts|proof)/|'
                      r'harness/(README\.md|SRC_LAYOUT\.md|queue\.py|loop\.sh|build-and-verify\.sh|'
                      r'update-matched-set\.sh)$|'
                      r'ghidra/(scripts|results)/|ghidra/(README\.md|REPORT\.md|build\.sh)$|'
                      r'analysis/(?!results/)|analysis/results/progress/|'
                      r'scripts/(check\.sh|progress\.py)$|[^/]+\.md$|pytest\.ini$)')

# Presets not built or run on pull requests, whatever the change. Paused by the owner on 2026-11-11
# while iterating: the runner does one job at a time and the cross builds dominated its queue.
# Empty the tuple to resume them. The release workflow does not read it and still builds every
# preset.
PAUSED_PRESETS = ('windows-x64', 'linux-x64')


def builds(preset, files):
    """True when the build job builds this preset for a change to these paths."""
    if preset in PAUSED_PRESETS:
        return False
    files = [f.strip() for f in files if f.strip()]
    if not files:
        return True
    if all(NO_BUILD.match(f) for f in files):
        return False
    if preset == DECOMP_PRESET:
        return True
    return not all(DECOMP_ONLY.match(f) or NO_BUILD.match(f) for f in files)


def skip_reason(preset, files):
    """Why the build job does not build this preset for a change to these paths; None when it does."""
    if preset in PAUSED_PRESETS:
        return 'paused on pull requests (PAUSED_PRESETS in oracle/presets/ci.py), whatever the change'
    if builds(preset, files):
        return None
    files = [f.strip() for f in files if f.strip()]
    if all(NO_BUILD.match(f) for f in files):
        return 'this pull request changes only paths no build reads (NO_BUILD in oracle/presets/ci.py)'
    return (f'this pull request changes only decompiled source and lift inputs, which {DECOMP_PRESET} '
            'alone builds (DECOMP_ONLY in oracle/presets/ci.py)')


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
        if preset in PAUSED_PRESETS:
            # Its build folder may still hold a client from before the pause; it is not run.
            log.append(f'::notice::oracle {mission}/{input_name}: {preset} is paused on pull requests '
                       '(PAUSED_PRESETS in oracle/presets/ci.py), so its client is not run')
            continue
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
    sub.add_parser('builds').add_argument('preset')
    r = sub.add_parser('run')
    r.add_argument('mission')
    r.add_argument('input')
    r.add_argument('--build-root', required=True)
    r.add_argument('--traces', required=True)
    r.add_argument('--env-file')
    r.add_argument('--changed', help='file listing the changed paths; a preset the build job '
                   'did not build for them is not run, since its client is an older build')
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

    if args.cmd == 'builds':
        files = sys.stdin.read().splitlines()
        reason = skip_reason(args.preset, files)
        if reason:
            print(f'::notice::skip: {args.preset}: {reason}', file=sys.stderr)
        print('no' if reason else 'yes')
        return 0

    if args.cmd == 'run':
        presets = PRESETS
        if args.changed:
            with open(args.changed, encoding='utf-8') as f:
                changed = f.read().splitlines()
            # A paused preset is left in for run_all, which logs why it is not run.
            presets = tuple(p for p in PRESETS if p in PAUSED_PRESETS or builds(p, changed))
            for p in PRESETS:
                if p not in presets:
                    print(f'::notice::oracle {args.mission}/{args.input}: {p} was not built for this '
                          'pull request, so its older client is not run')
        ran, failed, log = run_all(args.mission, args.input, args.build_root, args.traces, presets)
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
