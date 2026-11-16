#!/usr/bin/env python3
"""Compile the units the manifest claims with VC6, exactly as the harness compiles them for the
byte check: the decompilation's harness/lib/source.py compile_unit with the unit's profile from
its harness/units.json, in its toolchain container (toolchain/bin/cl). The decompilation is
$SFCOP_DECOMP_DIR, by default decomp/ in this repository (decomp/docs/INTERFACE.md).

    compile.py --manifest decomp/op/manifest.tsv --out DIR [--game DIR]

--game is the decompilation's source root (default $SFCOP_DECOMP_DIR/op).

Writes DIR/<unit path without .cpp>/<Unit>.obj and its /FAs listing (<Unit>.asm), which
objlift.py reads to find the symbol each annotation claims, and prints one `UNIT=OBJ` line per
unit, the form objlift.py's --obj takes. Exit status 2 when a unit fails to compile (its compiler
errors are printed).

A unit is compiled again only when its key changed: the SHA-256 of its source, its profile, every
file under the source tree that is not a .c or .cpp (any header it can include), and this script,
harness/lib/source.py and the compiler wrapper (DIR/<unit>.key holds it). A change to one unit's
.cpp recompiles that unit only; a change to any header recompiles them all.
"""
import argparse
import csv
import hashlib
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
DECOMP = os.path.abspath(os.environ.get('SFCOP_DECOMP_DIR') or os.path.join(REPO, 'decomp'))
if DECOMP not in sys.path:
    sys.path.insert(0, DECOMP)

from harness.lib import paths, source  # noqa: E402


def claimed_units(manifest):
    units = []
    with open(manifest, newline='') as f:
        for r in csv.DictReader(f, delimiter='\t'):
            if 'vc6' in r['modes'].split() and r['source'] not in units:
                units.append(r['source'])
    return units


def shared_key(game):
    """The part of every unit's key that is not the unit itself."""
    h = hashlib.sha256()
    for path in (os.path.abspath(__file__), os.path.join(DECOMP, 'harness', 'lib', 'source.py'),
                 paths.CL):
        h.update(path.encode() + b'\0')
        if os.path.isfile(path):
            with open(path, 'rb') as f:
                h.update(hashlib.sha256(f.read()).digest())
    for root, dirs, names in os.walk(game):
        dirs.sort()
        for n in sorted(names):
            if n.lower().endswith(('.c', '.cpp')) or n == 'manifest.tsv':
                continue
            p = os.path.join(root, n)
            h.update(os.path.relpath(p, game).encode() + b'\0')
            with open(p, 'rb') as f:
                h.update(hashlib.sha256(f.read()).digest())
    return h.hexdigest()


def unit_key(shared, unit, path, profile):
    h = hashlib.sha256(shared.encode())
    h.update(unit.encode() + b'\0' + json.dumps(profile, sort_keys=True).encode() + b'\0')
    with open(path, 'rb') as f:
        h.update(f.read())
    return h.hexdigest()


def read_text(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--manifest', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--game', default=os.path.join(DECOMP, 'op'))
    a = ap.parse_args(argv)
    cfg = source.load_units()
    failed = 0
    shared = shared_key(a.game)
    for unit in claimed_units(a.manifest):
        path = os.path.join(a.game, unit)
        try:
            _, profile = source.unit_profile(cfg, a.game, path)
        except source.SourceError as e:
            print(f'compile: {e}', file=sys.stderr)
            failed += 1
            continue
        dest = os.path.join(os.path.abspath(a.out), os.path.splitext(unit)[0])
        key = unit_key(shared, unit, path, profile)
        if read_text(dest + '.key') == key + '\n' and os.path.isfile(dest + '.obj'):
            print(f'{unit}={dest}.obj')
            print(f'compile: {unit} unchanged', file=sys.stderr)
            continue
        if os.path.exists(dest + '.key'):
            os.remove(dest + '.key')
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
        elif os.path.exists(dest + '.asm'):
            os.remove(dest + '.asm')
        with open(dest + '.key', 'w') as f:
            f.write(key + '\n')
        print(f'{unit}={obj}')
        print(f'compile: {unit} in {time.time() - t0:.1f} s (profile {profile["name"]})',
              file=sys.stderr)
    return 2 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
