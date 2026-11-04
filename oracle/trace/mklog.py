#!/usr/bin/env python3
"""Make a replay oracle trace from the oracle mission's log (oracle/trace/FORMAT.md).

    mklog.py LOG --mission NAME --input NAME --exe-sha256 HEX --build TEXT [--recorded TIME] [-o OUT]

LOG is OracleTrace.txt as the game wrote it (run_oracle.sh keeps it as trace.raw.txt). The trace
goes to OUT, or to standard output. --recorded defaults to the current UTC time.
Exit status: 0 written, 2 the log or an argument breaks the format.
"""
import argparse
import datetime
import hashlib
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import compare  # noqa: E402  (oracle/trace is not a package)

RECORDER_VERSION_LINE = '# oracle-trace v1'
END_REASONS = ('max_samples', 'one_side_gone', 'aborted')

# Field count and the keyword expected at each fixed position (0-based), per record kind.
_LAYOUT = {
    'S': (29, {3: 'grid', 6: 'raw', 19: 'spd', 21: 'dmg', 23: 'tgt', 25: 'dist', 27: 'st'}),
    'E': (18, {4: 'i', 10: 'f', 16: 'h'}),
    'END': (3, {}),
}
_SAMPLE = re.compile(r'(0|[1-9][0-9]*)\Z')


class LogError(Exception):
    """The mission log breaks the format. The message names the line."""


def normalise(data):
    """Return the log's record lines as (line number, text), with metadata and line ends removed."""
    try:
        text = data.decode('ascii')
    except UnicodeDecodeError as e:
        raise LogError(f'byte {e.start}: not ASCII') from None
    if not text:
        raise LogError('line 1: empty log')
    if not text.endswith('\n'):
        raise LogError(f'line {text.count(chr(10)) + 1}: last line has no LF (log cut short)')
    lines = text[:-1].split('\n')
    if lines[0].removesuffix('\r').rstrip(' \t') != RECORDER_VERSION_LINE:
        raise LogError(f'line 1: not "{RECORDER_VERSION_LINE}" (unsupported recorder version)')
    records = []
    for number, line in enumerate(lines, 1):
        line = line.removesuffix('\r')
        if '\r' in line:
            raise LogError(f'line {number}: CR inside a line')
        line = line.rstrip(' \t')
        if not line:
            raise LogError(f'line {number}: blank line')
        if line.startswith('#'):
            continue
        records.append((number, line))
    return records


def _sample_of(number, line):
    fields = line.split(' ')
    layout = _LAYOUT.get(fields[0])
    if layout is None:
        raise LogError(f'line {number}: unknown record "{fields[0]}"')
    count, keywords = layout
    if '' in fields:
        raise LogError(f'line {number}: fields not separated by exactly one space')
    if len(fields) != count:
        raise LogError(f'line {number}: {fields[0]} record has {len(fields)} fields, not {count}')
    for index, keyword in keywords.items():
        if fields[index] != keyword:
            raise LogError(f'line {number}: field {index + 1} is "{fields[index]}", not "{keyword}"')
    if not _SAMPLE.match(fields[1]):
        raise LogError(f'line {number}: sample "{fields[1]}" is not a decimal integer')
    if fields[0] == 'END' and fields[2] not in END_REASONS:
        raise LogError(f'line {number}: END reason "{fields[2]}" is not one of {", ".join(END_REASONS)}')
    return int(fields[1])


def tick_hashes(data):
    """Return the hash of every tick, 0 to the END sample, from the mission log's bytes."""
    records = normalise(data)
    groups = []
    end_seen = False
    previous = 0
    for number, line in records:
        if end_seen:
            raise LogError(f'line {number}: record after END')
        sample = _sample_of(number, line)
        if sample < previous:
            raise LogError(f'line {number}: sample {sample} after sample {previous}')
        previous = sample
        while len(groups) <= sample:
            groups.append([])
        groups[sample].append(line + '\n')
        end_seen = line.startswith('END ')
    if not end_seen:
        raise LogError('no END record (the run did not finish)')
    return [hashlib.sha256(''.join(group).encode('ascii')).hexdigest() for group in groups]


def make_trace(data, header):
    """Return the trace's text for the mission log `data` and the header dict (FORMAT.md order)."""
    for key in compare.HEADER_KEYS:
        problem = compare.check_header_value(key, header[key])
        if problem:
            raise LogError(f'header field {key}: {problem}')
    lines = [f'{key}\t{header[key]}\n' for key in compare.HEADER_KEYS]
    lines += [f'{tick}\t{digest}\n' for tick, digest in enumerate(tick_hashes(data))]
    return ''.join(lines)


def main(argv):
    parser = argparse.ArgumentParser(prog='mklog.py', description='Make an oracle trace from the mission log.')
    parser.add_argument('log')
    parser.add_argument('--mission', required=True)
    parser.add_argument('--input', required=True)
    parser.add_argument('--exe-sha256', required=True)
    parser.add_argument('--build', required=True)
    parser.add_argument('--recorded', help='UTC time YYYY-MM-DDTHH:MM:SSZ (default: now)')
    parser.add_argument('-o', '--output')
    try:
        args = parser.parse_args(argv)
    except SystemExit as e:
        return compare.EXIT_BAD_INPUT if e.code else compare.EXIT_OK
    recorded = args.recorded or datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
    header = {
        'format_version': str(compare.FORMAT_VERSION),
        'mission': args.mission,
        'input': args.input,
        'exe_sha256': args.exe_sha256,
        'build': args.build,
        'recorded': recorded,
    }
    try:
        data = pathlib.Path(args.log).read_bytes()
        text = make_trace(data, header)
    except OSError as e:
        print(f'mklog.py: {args.log}: {e.strerror}', file=sys.stderr)
        return compare.EXIT_BAD_INPUT
    except LogError as e:
        print(f'mklog.py: {args.log}: {e}', file=sys.stderr)
        return compare.EXIT_BAD_INPUT
    if args.output:
        pathlib.Path(args.output).write_bytes(text.encode('ascii'))
    else:
        sys.stdout.buffer.write(text.encode('ascii'))  # bytes, so Windows does not add CRs
    return compare.EXIT_OK


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
