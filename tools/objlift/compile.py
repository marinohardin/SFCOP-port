#!/usr/bin/env python3
"""Compile the units the manifest claims with VC6, exactly as the harness compiles them for the
byte check: harness/lib/source.py's compile_unit with the unit's profile from harness/units.json,
in the toolchain container (toolchain/bin/cl).

    compile.py --manifest game/manifest.tsv --out DIR [--game DIR]

Writes DIR/<unit path without .cpp>/<Unit>.obj and its /FAs listing (<Unit>.asm), which
objlift.py reads to find the symbol each annotation claims, and prints one `UNIT=OBJ` line per
unit, the form objlift.py's --obj takes. Exit status 2 when a unit fails to compile (its compiler
errors are printed).
"""
import argparse
import csv
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
if REPO not in sys.path:
    sys.path.insert(0, REPO)

from harness.lib import paths, source  # noqa: E402


def claimed_units(manifest):
    units = []
    with open(manifest, newline='') as f:
        for r in csv.DictReader(f, delimiter='\t'):
            if 'vc6' in r['modes'].split() and r['source'] not in units:
                units.append(r['source'])
    return units


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--manifest', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--game', default=os.path.join(REPO, 'game'))
    a = ap.parse_args(argv)
    cfg = source.load_units()
    failed = 0
    for unit in claimed_units(a.manifest):
        path = os.path.join(a.game, unit)
        try:
            _, profile = source.unit_profile(cfg, a.game, path)
        except source.SourceError as e:
            print(f'compile: {e}', file=sys.stderr)
            failed += 1
            continue
        dest = os.path.join(os.path.abspath(a.out), os.path.splitext(unit)[0])
        t0 = time.time()
        # The compiler container sees the work tree, not every directory: build there, then copy.
        build = source.compile_unit(path, profile, a.game,
                                    build_root=os.path.join(paths.STATE, 'objlift'))
        if not build['ok']:
            print(f'compile: {unit}: BUILD HAS FAILED ({build["secs"]} s)\n{build["log"]}',
                  file=sys.stderr)
            failed += 1
            continue
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        obj = dest + '.obj'
        shutil.copyfile(build['obj'], obj)
        if build['listing']:
            shutil.copyfile(build['listing'], dest + '.asm')
        print(f'{unit}={obj}')
        print(f'compile: {unit} in {time.time() - t0:.1f} s (profile {profile["name"]})',
              file=sys.stderr)
    return 2 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
