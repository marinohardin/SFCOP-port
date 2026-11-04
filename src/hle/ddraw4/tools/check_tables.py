#!/usr/bin/env python3
"""Checks the guest shim tables of the DirectDraw and Direct3D modules against src/hle/surface.tsv.

    python3 check_tables.py [--quiet]

Reads Ddraw4Guest.cpp and D3d3Guest.cpp, expands the macros their vtable tables are built from,
and for every interface compares, slot by slot, the method name and the stack bytes the shim
purges (the `RET` argument count times four) with the interface's rows in surface.tsv. The
DDRAW.DLL exports are compared by name. Prints one line with the number of entries compared and
exits 0, or lists every mismatch and exits 1. Standard library only.
"""
import argparse
import csv
import pathlib
import re
import sys

HLE = pathlib.Path(__file__).resolve().parent.parent.parent
GUEST_FILES = [HLE / 'ddraw4' / 'Ddraw4Guest.cpp', HLE / 'd3d3' / 'D3d3Guest.cpp']
SURFACE = HLE / 'surface.tsv'
# Interfaces whose rows the two modules own; the DLL's exports are checked by name.
DLL = 'DDRAW.DLL'

ENTRY = re.compile(r'\{\s*((?:"[^"]*"\s*)+),\s*shim<(.*)>\s*\}\s*$', re.S)


def join_continuations(text):
    return re.sub(r'\\\n', ' ', text)


def macros_of(text):
    """{name: (parameters, body)} for the function-like #defines."""
    found = {}
    for match in re.finditer(r'^#define\s+(\w+)\(([^)]*)\)[ \t]*(.*)$', text, re.M):
        params = [p.strip() for p in match.group(2).split(',') if p.strip()]
        found[match.group(1)] = (params, match.group(3))
    return found


def split_args(text):
    """Top-level comma separated arguments of a macro call, brackets respected."""
    args, depth, current = [], 0, ''
    for ch in text:
        if ch in '(<{':
            depth += 1
        elif ch in ')>}':
            depth -= 1
        if ch == ',' and depth == 0:
            args.append(current.strip())
            current = ''
        else:
            current += ch
    args.append(current.strip())
    return args


def expand(body, macros):
    """Expands macro calls in `body` until none is left."""
    changed = True
    while changed:
        changed = False
        for name, (params, definition) in macros.items():
            call = re.search(r'\b' + name + r'\(', body)
            if not call:
                continue
            start = call.end()
            depth, i = 1, start
            while depth:
                depth += {'(': 1, ')': -1}.get(body[i], 0)
                i += 1
            args = split_args(body[start:i - 1])
            result = definition
            for param, arg in zip(params, args):
                result = re.sub(r'\b' + re.escape(param) + r'\b', lambda _m, a=arg: a, result)
            body = body[:call.start()] + result + body[i:]
            changed = True
    return body


def split_entries(body):
    """The top-level {...} entries of a table body."""
    entries, depth, current = [], 0, ''
    for ch in body:
        if ch == '{':
            depth += 1
        if depth:
            current += ch
        if ch == '}':
            depth -= 1
            if depth == 0:
                entries.append(current)
                current = ''
    return entries


def parse_tables(path):
    """[(table name, [(interface, method, stack bytes)])] for the vtables of one guest file."""
    text = join_continuations(path.read_text())
    macros = macros_of(text)
    tables = []
    for match in re.finditer(r'const\s+win32hle_shim\s+(k\w+Methods)\[\]\s*=\s*\{', text):
        depth, i = 1, match.end()
        while depth:
            depth += {'{': 1, '}': -1}.get(text[i], 0)
            i += 1
        body = expand(text[match.end():i - 1], macros)
        entries = []
        for entry in split_entries(body):
            if 'nullptr' in entry:
                continue
            parsed = ENTRY.match(entry.strip())
            if not parsed:
                raise ValueError('%s: cannot read the table entry %s' % (path.name, entry))
            name = ''.join(re.findall(r'"([^"]*)"', parsed.group(1)))
            interface, method = re.fullmatch(r'sfcop!(\w+)::(\w+)', name).groups()
            stack = int(parsed.group(2).rsplit(',', 1)[1])
            entries.append((interface, method, stack))
        tables.append((match.group(1), entries))
    return tables


def exports_of(path):
    """The names in the file's `sfcop_hle_<module>[]` table."""
    text = path.read_text()
    match = re.search(r'extern "C" const win32hle_shim sfcop_hle_\w+\[\]\s*=\s*\{(.*?)\};', text, re.S)
    return re.findall(r'\{\s*"([^"]+)"', match.group(1)) if match else []


def surface_rows():
    with open(SURFACE, newline='') as f:
        rows = list(csv.DictReader(f, delimiter='\t'))
    by_module = {}
    for row in rows:
        by_module.setdefault(row['module'], []).append(row)
    return by_module


def check():
    """(entries compared, rows covered, [problems])."""
    rows = surface_rows()
    problems = []
    compared = 0
    covered = set()
    seen = set()
    for path in GUEST_FILES:
        for table, entries in parse_tables(path):
            interfaces = {e[0] for e in entries}
            if len(interfaces) != 1:
                problems.append('%s: table mixes interfaces %s' % (table, sorted(interfaces)))
                continue
            interface = interfaces.pop()
            seen.add(interface)
            expected = rows.get(interface, [])
            slots = sorted(expected, key=lambda r: int(r['ordinal_or_slot']))
            if len(slots) != len(entries):
                problems.append('%s: %d shims, %d rows in surface.tsv' % (interface, len(entries), len(slots)))
            for index, (entry, row) in enumerate(zip(entries, slots)):
                compared += 1
                covered.add((interface, row['name']))
                if int(row['ordinal_or_slot']) != index:
                    problems.append('%s::%s: row slot %s, table slot %d' % (interface, entry[1], row['ordinal_or_slot'], index))
                if row['name'] != entry[1]:
                    problems.append('%s slot %d: shim %s, row %s' % (interface, index, entry[1], row['name']))
                if row['stack_bytes'] != str(entry[2]):
                    problems.append('%s::%s: shim purges %d bytes, row says %s' % (interface, entry[1], entry[2], row['stack_bytes']))
                if row['status'] not in ('implemented', 'stub'):
                    problems.append('%s::%s: status %s' % (interface, entry[1], row['status']))
    for module in ('IDirect3D3', 'IDirect3DDevice3', 'IDirect3DTexture2', 'IDirect3DViewport3',
                   'IDirectDraw', 'IDirectDraw2', 'IDirectDraw4', 'IDirectDrawClipper',
                   'IDirectDrawPalette', 'IDirectDrawSurface3', 'IDirectDrawSurface4'):
        if module not in seen:
            problems.append('%s: no table' % module)
    exported = []
    for path in GUEST_FILES:
        exported += exports_of(path)
    dll = sorted(r['name'] for r in rows.get(DLL, []))
    if sorted(exported) != dll:
        problems.append('%s exports: shims %s, rows %s' % (DLL, sorted(exported), dll))
    compared += len(dll)
    return compared, sum(len(rows.get(m, [])) for m in seen) + len(dll), problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--quiet', action='store_true')
    args = parser.parse_args()
    compared, rows, problems = check()
    if problems:
        for problem in problems:
            print('MISMATCH', problem)
        return 1
    if not args.quiet:
        print('ok: %d shim table entries match their %d surface.tsv rows (names, slot order, stack bytes)'
              % (compared, rows))
    return 0


if __name__ == '__main__':
    sys.exit(main())
