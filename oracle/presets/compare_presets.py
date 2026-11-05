#!/usr/bin/env python3
"""Compare the replay-oracle traces the presets made (oracle/presets/README.md).

    compare_presets.py ROOT PRESET... [--mission NAME] [--input NAME] [--expected DIR]

Reads ROOT/<preset>/<mission>/<input>.trace for each PRESET (defaults: OracleSkirmish, skirmish),
compares every pair of presets with compare.py's rules, prints the SHA-256 of each trace's tick
lines (the header names the build and the time, so it is left out), and, when --expected names a
folder holding recorded traces, compares each preset's tree with it (`compare.py --tree`).

Exit status: 0 every pair identical, 4 a tick differs (the first one is printed), 2 a trace is
missing, truncated or malformed, a compared header field differs, or the command line is wrong.
A pair that differs in a header field is reported as 2 even when another pair differs in a tick.
"""
import argparse
import hashlib
import itertools
import pathlib
import shutil
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / 'trace'))
import compare  # noqa: E402  (oracle/trace is not a package)


def tick_digest(path):
    """SHA-256 of the trace's tick lines, or None when the file cannot be read as a trace."""
    try:
        header, ticks = compare.parse_trace(pathlib.Path(path).read_bytes())
    except (OSError, compare.TraceError):
        return None
    return hashlib.sha256(''.join(f'{i}\t{h}\n' for i, h in enumerate(ticks)).encode('ascii')).hexdigest()


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('root')
    p.add_argument('presets', nargs='+')
    p.add_argument('--mission', default='OracleSkirmish')
    p.add_argument('--input', default='skirmish')
    p.add_argument('--expected')
    try:
        args = p.parse_args(argv)
    except SystemExit:
        return compare.EXIT_BAD_INPUT
    if len(set(args.presets)) != len(args.presets):
        print('compare_presets: a preset is named twice', file=sys.stderr)
        return compare.EXIT_BAD_INPUT
    root = pathlib.Path(args.root)
    paths = {name: root / name / args.mission / f'{args.input}.trace' for name in args.presets}
    statuses = set()

    for name, path in paths.items():
        digest = tick_digest(path)
        if digest is None:
            # compare_files names the file and the line that breaks the format.
            status, message = compare.compare_files(path, path)
            print(f'{name}: {message}')
            statuses.add(compare.EXIT_BAD_INPUT)
        else:
            print(f'ticks-sha256 {name} {digest}')
    if compare.EXIT_BAD_INPUT in statuses:
        return compare.EXIT_BAD_INPUT

    if len(paths) < 2:
        print('only one preset: nothing to compare pairwise')
    for a, b in itertools.combinations(args.presets, 2):
        status, message = compare.compare_files(paths[a], paths[b])
        print(f'{a} vs {b}: {message}')
        statuses.add(status)

    if args.expected:
        # Only the mission under test: a view of the expected folder holding just its folder, so
        # compare.py --tree does not ask a menu-path run for the skirmish's recorded trace.
        view = pathlib.Path(tempfile.mkdtemp(prefix='oracle-expected-'))
        try:
            wanted = pathlib.Path(args.expected) / args.mission
            if wanted.is_dir():
                (view / args.mission).symlink_to(wanted.resolve())
            for name in args.presets:
                status, lines = compare.compare_tree(view, root / name)
                for line in lines:
                    print(f'{name} vs expected: {line}'.replace(str(view), args.expected))
                statuses.add(status)
        finally:
            shutil.rmtree(view, ignore_errors=True)

    if compare.EXIT_BAD_INPUT in statuses:
        return compare.EXIT_BAD_INPUT
    if compare.EXIT_MISMATCH in statuses:
        return compare.EXIT_MISMATCH
    return compare.EXIT_OK


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
