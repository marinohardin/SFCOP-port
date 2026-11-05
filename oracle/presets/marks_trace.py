#!/usr/bin/env python3
"""Make a replay-oracle trace of a menu path from the client's mark lines.

    marks_trace.py LOG --mission NAME --input NAME --exe-sha256 HEX --build TEXT
                   [--recorded TIME] [-o OUT] [--timings OUT]

LOG is the client's standard error. Each `[mark] reached 0x<address>[ (<name>)] at <t> s` line is
one tick, in the order the marks were reached; a mark seen twice counts once. The tick's hash is the
SHA-256 of the record `mark 0x<ADDRESS> <name>` plus LF (address in upper-case hex, name `-` when
the mark has none), and a last tick holds the record `end`. Times are not hashed: they differ from
run to run and from machine to machine. --timings writes them (`<tick>TAB<seconds>`) beside the
trace for people to read.

The result is an ordinary trace (oracle/trace/FORMAT.md), so compare.py compares it. Exit status: 0
written, 2 the log or an argument breaks the format, or no mark was reached.
"""
import argparse
import datetime
import hashlib
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / 'trace'))
import compare  # noqa: E402  (oracle/trace is not a package)

_MARK = re.compile(r'\[mark\] reached 0x([0-9A-Fa-f]{1,8})(?: \(([^)]*)\))? at ([0-9]+(?:\.[0-9]+)?) s')


def marks(text):
    """Return [(ADDRESS, name, seconds)] in the order first reached."""
    seen = set()
    found = []
    for line in text.splitlines():
        m = _MARK.search(line)
        if not m:
            continue
        address = '0x%08X' % int(m.group(1), 16)
        if address in seen:
            continue
        seen.add(address)
        found.append((address, (m.group(2) or '-').strip() or '-', m.group(3)))
    return found


def record_hash(record):
    return hashlib.sha256((record + '\n').encode('ascii')).hexdigest()


def make_trace(found, mission, input_name, exe_sha256, build, recorded):
    lines = ['format_version\t1', f'mission\t{mission}', f'input\t{input_name}',
             f'exe_sha256\t{exe_sha256}', f'build\t{build}', f'recorded\t{recorded}']
    records = [f'mark {a} {n}' for a, n, _ in found] + ['end']
    lines += [f'{i}\t{record_hash(r)}' for i, r in enumerate(records)]
    return '\n'.join(lines) + '\n'


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    p.add_argument('log')
    p.add_argument('--mission', required=True)
    p.add_argument('--input', required=True)
    p.add_argument('--exe-sha256', required=True)
    p.add_argument('--build', required=True)
    p.add_argument('--recorded')
    p.add_argument('-o', '--out')
    p.add_argument('--timings')
    try:
        args = p.parse_args(argv)
    except SystemExit:
        return compare.EXIT_BAD_INPUT
    recorded = args.recorded or datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
    for key, value in (('mission', args.mission), ('input', args.input), ('exe_sha256', args.exe_sha256),
                       ('build', args.build), ('recorded', recorded)):
        bad = compare.check_header_value(key, value)
        if bad:
            print(f'marks_trace: {key}: {bad}', file=sys.stderr)
            return compare.EXIT_BAD_INPUT
    try:
        text = pathlib.Path(args.log).read_bytes().decode('utf-8', 'replace')
    except OSError as e:
        print(f'marks_trace: {args.log}: {e.strerror}', file=sys.stderr)
        return compare.EXIT_BAD_INPUT
    found = marks(text)
    if not found:
        print(f'marks_trace: {args.log}: no mark was reached', file=sys.stderr)
        return compare.EXIT_BAD_INPUT
    out = make_trace(found, args.mission, args.input, args.exe_sha256, args.build, recorded)
    if args.out:
        pathlib.Path(args.out).write_text(out, encoding='ascii', newline='\n')
    else:
        sys.stdout.write(out)
    if args.timings:
        rows = ''.join(f'{i}\t{t}\n' for i, (_, _, t) in enumerate(found))
        pathlib.Path(args.timings).write_text(rows, encoding='ascii', newline='\n')
    return compare.EXIT_OK


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
