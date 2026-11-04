"""Tests for the replay oracle's trace format, its writer and its comparator (oracle/trace/).

Fixtures under oracle/trace/fixtures/ are invented. The trace pairs use made-up hashes: tick t is
"a<t>" repeated 32 times, and "b<t>" where the build under test differs. The mission log
oracle_log.txt is an invented two-ship run; the hashes in oracle_log.trace were taken with
`shasum -a 256` over the per-tick records written out in test_mission_log_becomes_hand_hashed_trace.
"""
import importlib.util
import pathlib
import subprocess
import sys

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]
TRACE_DIR = ROOT / 'oracle' / 'trace'
FIXTURES = TRACE_DIR / 'fixtures'


def _load(name):
    sys.path.insert(0, str(TRACE_DIR))
    try:
        spec = importlib.util.spec_from_file_location(name, TRACE_DIR / f'{name}.py')
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return mod
    finally:
        sys.path.remove(str(TRACE_DIR))


compare = _load('compare')
mklog = _load('mklog')

EXPECTED = FIXTURES / 'expected.trace'
ORIG_EXE = '0123456789abcdef' * 4
EMPTY_SHA256 = 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'

HEADER = ('format_version\t1\nmission\tOracleSkirmish\ninput\tskirmish\n'
          f'exe_sha256\t{ORIG_EXE}\nbuild\toriginal 2.5.5.2\nrecorded\t2026-01-02T03:04:05Z\n')
TICK0 = '0\t' + 'a0' * 32 + '\n'
RAW = ('2129587950 2129587950 2129587950 2129587950 2129587950 '
       '-98765.4375 -98765.4375 -98765.4375 -98765.4375 -98765.4375')


def run(*args):
    return compare.main([str(a) for a in args])


# --- comparator: the cases of the package's acceptance line --------------------------------------

def test_matching_pair_is_identical_and_exits_0(capsys):
    assert run(EXPECTED, FIXTURES / 'actual_same.trace') == 0
    assert capsys.readouterr().out == 'identical: 6 ticks\n'


def test_exe_hash_build_and_date_are_not_compared():
    # actual_same.trace differs from expected.trace only in exe_sha256, build and recorded.
    assert run(EXPECTED, FIXTURES / 'actual_same.trace') == 0


def test_mismatch_reports_first_differing_tick_and_exits_4(capsys):
    assert run(EXPECTED, FIXTURES / 'actual_tick3.trace') == 4
    assert capsys.readouterr().out == (
        'first difference at tick 3: expected ' + 'a3' * 32 + ', actual ' + 'b3' * 32 + '\n')


def test_header_mismatch_names_the_field_and_exits_2(capsys):
    assert run(EXPECTED, FIXTURES / 'actual_header_input.trace') == 2
    assert capsys.readouterr().out == (
        'header mismatch in field input: expected skirmish, actual skirmish_slow\n')


def test_truncated_actual_differs_at_its_first_missing_tick(capsys):
    assert run(EXPECTED, FIXTURES / 'actual_truncated.trace') == 4
    assert capsys.readouterr().out == (
        'first difference at tick 3: expected ' + 'a3' * 32 + ', actual missing\n')


def test_actual_longer_than_expected_differs_at_first_extra_tick(capsys):
    assert run(FIXTURES / 'actual_truncated.trace', EXPECTED) == 4
    assert capsys.readouterr().out == (
        'first difference at tick 3: expected missing, actual ' + 'a3' * 32 + '\n')


def test_comparator_process_exits_4_on_mismatch():
    result = subprocess.run(
        [sys.executable, str(TRACE_DIR / 'compare.py'), str(EXPECTED), str(FIXTURES / 'actual_tick3.trace')],
        capture_output=True, text=True, check=False)
    assert result.returncode == 4
    assert result.stdout.startswith('first difference at tick 3: ')


def test_every_fixture_trace_is_well_formed():
    for path in sorted(FIXTURES.glob('*.trace')):
        header, hashes = compare.parse_trace(path.read_bytes())
        assert header['mission'] == 'OracleSkirmish', path.name
        assert hashes, path.name


# --- comparator: malformed files ------------------------------------------------------------------

@pytest.mark.parametrize('text, message', [
    ('', 'line 1: empty file'),
    (HEADER + TICK0.rstrip('\n'), 'line 7: last line has no LF'),
    (HEADER.replace('\n', '\r\n', 1) + TICK0, 'line 1: CR in file (LF line endings only)'),
    (HEADER, 'line 7: no tick lines'),
    (HEADER.replace('format_version\t1', 'format_version\t2') + TICK0,
     'line 1: field format_version: unsupported version 2 (this comparator reads 1)'),
    (HEADER.replace('mission\tOracleSkirmish\ninput\tskirmish', 'input\tskirmish\nmission\tOracleSkirmish') + TICK0,
     'line 2: field mission: found "input" instead'),
    (HEADER.replace(ORIG_EXE, ORIG_EXE.upper()) + TICK0, 'line 4: field exe_sha256: not 64 lowercase hex digits'),
    (HEADER.replace('2026-01-02T03:04:05Z', '2026-01-02 03:04:05') + TICK0,
     'line 6: field recorded: not a UTC time YYYY-MM-DDTHH:MM:SSZ'),
    (HEADER.replace('build\toriginal 2.5.5.2', 'build original 2.5.5.2') + TICK0,
     'line 5: field build: not "build<TAB>value"'),
    ('format_version\t1\nmission\tOracleSkirmish\n', 'line 3: field input: missing'),
    (HEADER + TICK0 + '2\t' + 'a2' * 32 + '\n', 'line 8: tick 2 where tick 1 was due'),
    (HEADER + '00\t' + 'a0' * 32 + '\n', 'line 7: tick "00" is not a decimal integer'),
    (HEADER + '0\t' + 'a0' * 31 + '\n', 'line 7: hash of tick 0 is not 64 lowercase hex digits'),
    (HEADER + TICK0 + '\n', 'line 8: not "tick<TAB>hash"'),
    (HEADER.replace('original', 'originäl') + TICK0, 'byte 143: not ASCII'),
])
def test_malformed_trace_is_rejected_with_line_and_field(text, message):
    with pytest.raises(compare.TraceError) as caught:
        compare.parse_trace(text.encode('utf-8'))
    assert str(caught.value) == message


def test_malformed_actual_exits_2_naming_the_file(tmp_path, capsys):
    bad = tmp_path / 'bad.trace'
    bad.write_bytes(HEADER.encode('ascii'))
    assert run(EXPECTED, bad) == 2
    assert capsys.readouterr().out == f'ACTUAL {bad}: line 7: no tick lines\n'


def test_missing_expected_file_exits_2(tmp_path, capsys):
    assert run(tmp_path / 'none.trace', EXPECTED) == 2
    assert capsys.readouterr().out.startswith(f'EXPECTED {tmp_path / "none.trace"}: ')


def test_wrong_command_line_exits_2(capsys):
    assert run(EXPECTED) == 2
    assert capsys.readouterr().err.startswith('usage: compare.py')


# --- comparator: a tree of expected traces --------------------------------------------------------

def _tree(tmp_path, actual_fixture):
    expected = tmp_path / 'expected' / 'OracleSkirmish'
    actual = tmp_path / 'actual' / 'OracleSkirmish'
    expected.mkdir(parents=True)
    actual.mkdir(parents=True)
    (expected / 'skirmish.trace').write_bytes(EXPECTED.read_bytes())
    if actual_fixture:
        (actual / 'skirmish.trace').write_bytes((FIXTURES / actual_fixture).read_bytes())
    return tmp_path / 'expected', tmp_path / 'actual'


def test_tree_with_matching_trace_exits_0(tmp_path, capsys):
    assert run('--tree', *_tree(tmp_path, 'actual_same.trace')) == 0
    assert capsys.readouterr().out == 'OracleSkirmish/skirmish.trace: identical: 6 ticks\n'


def test_tree_with_differing_trace_exits_4(tmp_path, capsys):
    assert run('--tree', *_tree(tmp_path, 'actual_tick3.trace')) == 4
    assert capsys.readouterr().out.startswith('OracleSkirmish/skirmish.trace: first difference at tick 3: ')


def test_tree_with_missing_actual_trace_exits_2(tmp_path):
    assert run('--tree', *_tree(tmp_path, None)) == 2


def test_tree_with_empty_expected_root_exits_0(tmp_path, capsys):
    (tmp_path / 'expected').mkdir()
    assert run('--tree', tmp_path / 'expected', tmp_path) == 0
    assert capsys.readouterr().out == f'no expected traces under {tmp_path / "expected"}\n'


def test_tree_with_missing_expected_root_exits_2_naming_it(tmp_path, capsys):
    assert run('--tree', tmp_path / 'nothing', tmp_path) == 2
    assert capsys.readouterr().out == f'EXPECTED_ROOT {tmp_path / "nothing"}: not a directory\n'


# --- trace writer: mission log to trace -----------------------------------------------------------

def test_mission_log_becomes_hand_hashed_trace(tmp_path):
    # The records after normalisation (CRLF and trailing blanks removed, '#' lines dropped),
    # grouped by sample, each line followed by LF:
    #   tick 0: S 0 c101 ... st 2 / E 0 01000000 c101 i 1 30 30 0 0 f 0 0 0 0 0 h - / S 0 c201 ... st 2
    #   tick 1: S 1 c101 ... / S 1 c201 ...
    #   tick 2: E 2 00000080 c101 i 0 1 0 0 0 f 12.5 0 0 0 0 h c201 / S 2 c101 ... / S 2 c201 ...
    #   tick 3: END 3 max_samples
    out = tmp_path / 'out.trace'
    status = mklog.main([
        str(FIXTURES / 'oracle_log.txt'), '--mission', 'OracleSkirmish', '--input', 'skirmish',
        '--exe-sha256', ORIG_EXE, '--build', 'original 2.5.5.2', '--recorded', '2026-01-02T03:04:05Z',
        '-o', str(out)])
    assert status == 0
    assert out.read_bytes() == (FIXTURES / 'oracle_log.trace').read_bytes()


def _log(*records):
    return ('# oracle-trace v1\r\n' + ''.join(r + '\r\n' for r in records)).encode('ascii')


S0 = f'S 0 c101 grid 6 20 raw 6 20 {RAW} spd 0 dmg 0 tgt id-1 dist 64.5 st 2'


def test_sample_without_records_hashes_the_empty_record():
    # printf 'END 2 one_side_gone\n' | shasum -a 256
    end_hash = '663cf6dda37ffc98226abcc74654556c1ddb3dbea91436da61da80ebc425a4e7'
    hashes = mklog.tick_hashes(_log(S0, 'END 2 one_side_gone'))
    assert hashes[1:] == [EMPTY_SHA256, end_hash]
    assert len(hashes) == 3


def test_metadata_line_ends_and_trailing_blanks_do_not_change_hashes():
    plain = mklog.tick_hashes(b'# oracle-trace v1\n' + S0.encode() + b'\nEND 1 max_samples\n')
    noisy = mklog.tick_hashes(b'# oracle-trace v1\r\n# rng repin crt_srand=0x00400000\r\n'
                              + S0.encode() + b' \t\r\nEND 1 max_samples\r\n')
    assert plain == noisy


@pytest.mark.parametrize('data, message', [
    (b'', 'line 1: empty log'),
    (b'# oracle-trace v2\r\nEND 0 max_samples\r\n', 'line 1: not "# oracle-trace v1" (unsupported recorder version)'),
    (_log(S0), 'no END record (the run did not finish)'),
    (_log(S0, 'END 1 max_samples')[:-2], 'line 3: last line has no LF (log cut short)'),
    (_log(S0, 'END 1 max_samples', S0), 'line 4: record after END'),
    (_log(S0.replace('S 0', 'S 2'), S0, 'END 3 max_samples'), 'line 3: sample 0 after sample 2'),
    (_log(S0.replace(' spd ', ' speed ')), 'line 2: field 20 is "speed", not "spd"'),
    (_log(S0 + ' extra'), 'line 2: S record has 30 fields, not 29'),
    (_log('E 0 00000080 c101 i 0 1 0 0 0 f 0 0 0 0 0 h'), 'line 2: E record has 17 fields, not 18'),
    (_log(S0.replace('grid 6', 'grid  6')), 'line 2: fields not separated by exactly one space'),
    (_log('X 0 something'), 'line 2: unknown record "X"'),
    (_log('END -1 max_samples'), 'line 2: sample "-1" is not a decimal integer'),
    (_log('END 0 crashed'), 'line 2: END reason "crashed" is not one of max_samples, one_side_gone, aborted'),
    (_log(S0, '', 'END 1 max_samples'), 'line 3: blank line'),
    (b'# oracle-trace v1\r\nEND 0 max\rsamples\r\n', 'line 2: CR inside a line'),
])
def test_malformed_mission_log_is_rejected_with_line(data, message):
    with pytest.raises(mklog.LogError) as caught:
        mklog.tick_hashes(data)
    assert str(caught.value) == message


def test_writer_rejects_an_invalid_header_value(tmp_path, capsys):
    status = mklog.main([
        str(FIXTURES / 'oracle_log.txt'), '--mission', 'Oracle Skirmish', '--input', 'skirmish',
        '--exe-sha256', ORIG_EXE, '--build', 'original 2.5.5.2', '--recorded', '2026-01-02T03:04:05Z',
        '-o', str(tmp_path / 'out.trace')])
    assert status == 2
    assert 'header field mission: not a name' in capsys.readouterr().err
    assert not (tmp_path / 'out.trace').exists()
