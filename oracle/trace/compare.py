#!/usr/bin/env python3
"""Compare two replay oracle traces (oracle/trace/FORMAT.md).

    compare.py EXPECTED ACTUAL
    compare.py --tree EXPECTED_ROOT ACTUAL_ROOT

Exit status: 0 identical, 4 a tick differs (the first one is reported), 2 a compared header field
differs, a file is missing or malformed, or the command line is wrong.
"""
import datetime
import pathlib
import re
import sys

FORMAT_VERSION = 1
HEADER_KEYS = ('format_version', 'mission', 'input', 'exe_sha256', 'build', 'recorded')
COMPARED_KEYS = ('format_version', 'mission', 'input')

EXIT_OK = 0
EXIT_BAD_INPUT = 2
EXIT_MISMATCH = 4

_NAME = re.compile(r'[A-Za-z0-9_-]+\Z')
_HASH = re.compile(r'[0-9a-f]{64}\Z')
_TICK = re.compile(r'(0|[1-9][0-9]*)\Z')
_PRINTABLE = re.compile(r'[\x20-\x7e]{1,200}\Z')


class TraceError(Exception):
    """A trace breaks the format. The message names the line and, for the header, the field."""


def check_header_value(key, value):
    """Return None when `value` is valid for header field `key`, else what is wrong with it."""
    if key == 'format_version':
        if not _TICK.match(value):
            return 'not a decimal integer'
        if int(value) != FORMAT_VERSION:
            return f'unsupported version {value} (this comparator reads {FORMAT_VERSION})'
    elif key in ('mission', 'input'):
        if not _NAME.match(value):
            return 'not a name of letters, digits, "_" and "-"'
    elif key == 'exe_sha256':
        if not _HASH.match(value):
            return 'not 64 lowercase hex digits'
    elif key == 'build':
        if not _PRINTABLE.match(value) or value != value.strip(' '):
            return 'not 1 to 200 printable characters without leading or trailing space'
    elif key == 'recorded':
        try:
            datetime.datetime.strptime(value, '%Y-%m-%dT%H:%M:%SZ')
        except ValueError:
            return 'not a UTC time YYYY-MM-DDTHH:MM:SSZ'
        if len(value) != 20:
            return 'not a UTC time YYYY-MM-DDTHH:MM:SSZ'
    return None


def parse_trace(data):
    """Parse trace bytes into (header dict, list of tick hashes). Raises TraceError."""
    try:
        text = data.decode('ascii')
    except UnicodeDecodeError as e:
        raise TraceError(f'byte {e.start}: not ASCII') from None
    if '\r' in text:
        line = text[:text.index('\r')].count('\n') + 1
        raise TraceError(f'line {line}: CR in file (LF line endings only)')
    if not text:
        raise TraceError('line 1: empty file')
    if not text.endswith('\n'):
        raise TraceError(f'line {text.count(chr(10)) + 1}: last line has no LF')
    lines = text[:-1].split('\n')

    header = {}
    for number, key in enumerate(HEADER_KEYS, 1):
        if number > len(lines):
            raise TraceError(f'line {number}: field {key}: missing')
        parts = lines[number - 1].split('\t')
        if len(parts) != 2:
            raise TraceError(f'line {number}: field {key}: not "{key}<TAB>value"')
        if parts[0] != key:
            raise TraceError(f'line {number}: field {key}: found "{parts[0]}" instead')
        problem = check_header_value(key, parts[1])
        if problem:
            raise TraceError(f'line {number}: field {key}: {problem}')
        header[key] = parts[1]

    hashes = []
    for number, line in enumerate(lines[len(HEADER_KEYS):], len(HEADER_KEYS) + 1):
        parts = line.split('\t')
        if len(parts) != 2:
            raise TraceError(f'line {number}: not "tick<TAB>hash"')
        tick, digest = parts
        if not _TICK.match(tick):
            raise TraceError(f'line {number}: tick "{tick}" is not a decimal integer')
        if int(tick) != len(hashes):
            raise TraceError(f'line {number}: tick {tick} where tick {len(hashes)} was due')
        if not _HASH.match(digest):
            raise TraceError(f'line {number}: hash of tick {tick} is not 64 lowercase hex digits')
        hashes.append(digest)
    if not hashes:
        raise TraceError(f'line {len(lines) + 1}: no tick lines')
    return header, hashes


def compare(expected, actual):
    """Compare two parsed traces. Returns (exit status, one-line message)."""
    exp_header, exp_hashes = expected
    act_header, act_hashes = actual
    for key in COMPARED_KEYS:
        if exp_header[key] != act_header[key]:
            return EXIT_BAD_INPUT, (f'header mismatch in field {key}: '
                                    f'expected {exp_header[key]}, actual {act_header[key]}')
    for tick in range(max(len(exp_hashes), len(act_hashes))):
        exp = exp_hashes[tick] if tick < len(exp_hashes) else 'missing'
        act = act_hashes[tick] if tick < len(act_hashes) else 'missing'
        if exp != act:
            return EXIT_MISMATCH, f'first difference at tick {tick}: expected {exp}, actual {act}'
    return EXIT_OK, f'identical: {len(exp_hashes)} ticks'


def _load(role, path):
    try:
        data = pathlib.Path(path).read_bytes()
    except OSError as e:
        raise TraceError(f'{role} {path}: {e.strerror}') from None
    try:
        return parse_trace(data)
    except TraceError as e:
        raise TraceError(f'{role} {path}: {e}') from None


def compare_files(expected_path, actual_path):
    """Compare two trace files. Returns (exit status, one-line message)."""
    try:
        expected = _load('EXPECTED', expected_path)
        actual = _load('ACTUAL', actual_path)
    except TraceError as e:
        return EXIT_BAD_INPUT, str(e)
    return compare(expected, actual)


def compare_tree(expected_root, actual_root):
    """Compare every <mission>/<input>.trace under expected_root with its twin under actual_root.

    Returns (exit status, list of result lines).
    """
    expected_root = pathlib.Path(expected_root)
    actual_root = pathlib.Path(actual_root)
    if not expected_root.is_dir():
        # A mistyped root must not pass the oracle silently.
        return EXIT_BAD_INPUT, [f'EXPECTED_ROOT {expected_root}: not a directory']
    paths = sorted(expected_root.glob('*/*.trace'))
    if not paths:
        return EXIT_OK, [f'no expected traces under {expected_root}']
    results = []
    statuses = set()
    for path in paths:
        relative = path.relative_to(expected_root)
        status, message = compare_files(path, actual_root / relative)
        statuses.add(status)
        results.append(f'{relative.as_posix()}: {message}')
    if EXIT_BAD_INPUT in statuses:
        return EXIT_BAD_INPUT, results
    if EXIT_MISMATCH in statuses:
        return EXIT_MISMATCH, results
    return EXIT_OK, results


USAGE = 'usage: compare.py EXPECTED ACTUAL\n       compare.py --tree EXPECTED_ROOT ACTUAL_ROOT'


def main(argv):
    if len(argv) == 3 and argv[0] == '--tree':
        status, lines = compare_tree(argv[1], argv[2])
        for line in lines:
            print(line)
        return status
    if len(argv) == 2 and not any(a.startswith('-') for a in argv):
        status, message = compare_files(argv[0], argv[1])
        print(message)
        return status
    print(USAGE, file=sys.stderr)
    return EXIT_BAD_INPUT


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
