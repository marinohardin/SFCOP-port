#!/usr/bin/env python3
"""Compare the game's import table with what pcrecomp's win32hle answers.

    python3 imports_vs_hle.py <exe> <pcrecomp/runtime/win32hle> <out.tsv>

win32hle registers shims by name only (win32hle_shim {name, fn} tables, plus
SHIM2(ordinal, name, fn) in wsock32.c), so a game import counts as covered when
its name appears in any table. Ordinal-only imports are matched against SHIM2
ordinals for WSOCK32 and
"dll#N" table entries (dsound.dll#1) for the rest. Writes one row per import: dll, name, covered, hle_file.
Needs pefile.
"""
import collections
import os
import re
import sys

import pefile

SHIM_RE = re.compile(r'\{\s*"([A-Za-z_][A-Za-z0-9_@.#]*)"\s*,')
SHIM2_RE = re.compile(r'SHIM2\(\s*(\d+)\s*,\s*"([A-Za-z_][A-Za-z0-9_]*)"')


def hle_names(hle_dir):
    names, ordinals = {}, {}
    for fn in sorted(os.listdir(hle_dir)):
        if not fn.endswith('.c') or 'selftest' in fn:
            continue
        src = open(os.path.join(hle_dir, fn), encoding='utf-8', errors='replace').read()
        for m in SHIM_RE.finditer(src):
            names.setdefault(m.group(1), fn)
        for m in SHIM2_RE.finditer(src):
            names.setdefault(m.group(2), fn)
            ordinals[int(m.group(1))] = m.group(2)
    return names, ordinals


def main():
    exe, hle_dir, out = sys.argv[1:4]
    names, ordinals = hle_names(hle_dir)
    pe = pefile.PE(exe, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
    rows = []
    for d in pe.DIRECTORY_ENTRY_IMPORT:
        dll = d.dll.decode().upper()
        for imp in d.imports:
            if imp.name:
                name = imp.name.decode()
            elif dll.startswith('WSOCK32') and imp.ordinal in ordinals:
                name = ordinals[imp.ordinal]
            else:
                name = '#%d' % imp.ordinal
            hit = names.get(name) or names.get('%s#%d' % (dll.lower(), imp.ordinal or 0))
            rows.append((dll, name, 'yes' if hit else 'no', hit or ''))
    with open(out, 'w') as f:
        f.write('dll\tname\tcovered\thle_file\n')
        for r in rows:
            f.write('\t'.join(r) + '\n')
    per = collections.OrderedDict()
    for dll, _, cov, _ in rows:
        t = per.setdefault(dll, [0, 0])
        t[0] += 1
        t[1] += cov == 'yes'
    print('%-14s %6s %8s %8s' % ('dll', 'imports', 'covered', 'missing'))
    for dll, (n, c) in per.items():
        print('%-14s %6d %8d %8d' % (dll, n, c, n - c))
    n = len(rows)
    c = sum(r[2] == 'yes' for r in rows)
    print('%-14s %6d %8d %8d' % ('TOTAL', n, c, n - c))
    print('hle shim names known: %d' % len(names))


if __name__ == '__main__':
    main()
