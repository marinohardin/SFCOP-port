#!/usr/bin/env python3
"""Print where the build finds the original exe and the work directory, as CMake `set()` lines.

The locations come from harness/lib/paths.py, so the build and the harness agree on them and on
their overrides (SFCOP_REFERENCE, HARNESS_EXE, SFCOP_ROOT, SFCOP_WORK). The expected hash is the
exe's line in analysis/results/retail_hashes.sha256.

    python3 cmake/lifter/locate.py REPO
"""
import os
import sys


def main(argv):
    if len(argv) != 2:
        print('usage: locate.py REPO', file=sys.stderr)
        return 2
    sys.path.insert(0, os.path.abspath(argv[1]))
    from harness.lib import paths  # noqa: E402 -- the repository is only known at run time

    want = ''
    with open(paths.HASHES) as f:
        for line in f:
            digest, _, rel = line.rstrip('\n').partition('  ')
            if rel == paths.EXE_REL:
                want = digest
    for name, value in (('SFCOP_FOUND_EXE', paths.EXE), ('SFCOP_FOUND_EXE_SHA256', want),
                        ('SFCOP_FOUND_WORK', paths.WORK)):
        print('set(%s "%s")' % (name, value.replace('\\', '/').replace('"', '\\"')))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
