"""Tests for the oracle's per-preset runner and the cross-preset comparison (oracle/presets/).

The client is a stand-in shell script that prints mark lines (or writes the mission log of
oracle/trace/fixtures) and exits; no game, no emulator, no network. The data folder is an invented
folder holding one invented file. Expected hashes are written out by hand with hashlib over the
literal records, not taken from the code under test.
"""
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]
PRESETS = ROOT / 'oracle' / 'presets'
FIXTURES = ROOT / 'oracle' / 'trace' / 'fixtures'
RUN = PRESETS / 'run_preset.sh'
COMPARE = PRESETS / 'compare_presets.py'

MARKS_STDERR = (
    '[boot] something unrelated\n'
    '[mark] reached 0x00401000 (WinMain) at 1.250 s\n'
    '[mark] reached 0x0047C100 (Select Game) at 12.500 s\n'
    '[mark] reached 0x0047C155 (Setup) at 13.100 s\n'
    '[mark] reached 0x0047C100 (Select Game) at 14.000 s\n'
)


def sha(text):
    return hashlib.sha256(text.encode('ascii')).hexdigest()


def stand_in(path, body):
    path.write_text('#!/bin/sh\n' + body)
    path.chmod(0o755)


@pytest.fixture
def world(tmp_path):
    """A build root with a macos-arm64 stand-in client, an install folder and a data folder."""
    build = tmp_path / 'builds' / 'macos-arm64'
    (build / 'gameroot').mkdir(parents=True)
    stand_in(build / 'sfcop', "cat >&2 <<'EOF'\n" + MARKS_STDERR + "EOF\nexit 0\n")
    data = tmp_path / 'data'
    (data / 'logs').mkdir(parents=True)
    (data / 'registry.tsv').write_text('invented\n')
    env = dict(os.environ, SFCOP_BUILD_ROOT=str(tmp_path / 'builds'), SFCOP_ORACLE_DATA=str(data))
    env.pop('SFCOP_INSTALL', None)
    env.pop('SFCOP_ORACLE_EMULATOR', None)
    return tmp_path, build, data, env


def run_preset(world_, *args, env=None):
    tmp, _, _, base_env = world_
    return subprocess.run(['sh', str(RUN), *args, '--root', str(tmp / 'traces')],
                          env=env or base_env, capture_output=True, text=True)


# --- the runner -------------------------------------------------------------------------------

def test_menu_path_trace_has_one_tick_per_mark_and_an_end_tick(world):
    tmp, _, _, _ = world
    r = run_preset(world, 'macos-arm64')
    assert r.returncode == 0, r.stderr
    lines = (tmp / 'traces' / 'macos-arm64' / 'MenuPath' / 'menu.trace').read_text().split('\n')
    assert lines[:3] == ['format_version\t1', 'mission\tMenuPath', 'input\tmenu']
    assert lines[4].startswith('build\tnative macos-arm64 ')
    ticks = lines[6:-1]
    # A mark seen twice counts once; times are not part of the hash.
    assert ticks == [
        '0\t' + sha('mark 0x00401000 WinMain\n'),
        '1\t' + sha('mark 0x0047C100 Select Game\n'),
        '2\t' + sha('mark 0x0047C155 Setup\n'),
        '3\t' + sha('end\n'),
    ]
    assert lines[-1] == ''
    timings = (tmp / 'traces' / 'macos-arm64' / 'MenuPath' / 'menu.timings').read_text()
    assert timings == '0\t1.250\n1\t12.500\n2\t13.100\n'


def test_the_runner_leaves_the_data_folder_alone_and_never_prints_its_path(world):
    tmp, build, data, _ = world
    r = run_preset(world, 'macos-arm64')
    assert r.returncode == 0
    assert str(data) not in r.stdout + r.stderr
    assert sorted(p.name for p in data.iterdir()) == ['logs', 'registry.tsv']
    assert (data / 'registry.tsv').read_text() == 'invented\n'
    assert not (build / 'oracle-scratch' / 'macos-arm64').exists()
    assert not any('registry' in p.name for p in (tmp / 'traces').rglob('*'))


def test_the_client_gets_a_copy_of_the_data_folder(world):
    tmp, build, data, _ = world
    stand_in(build / 'sfcop',
             'while [ $# -gt 0 ]; do [ "$1" = --data ] && d=$2; shift; done\n'
             'echo "[mark] reached 0x00401000 (WinMain) at 1.0 s" >&2\n'
             '[ "$d" != "' + str(data) + '" ] && [ -f "$d/registry.tsv" ] || exit 7\n'
             'echo changed > "$d/registry.tsv"\n')
    r = run_preset(world, 'macos-arm64')
    assert r.returncode == 0, r.stderr
    assert (data / 'registry.tsv').read_text() == 'invented\n'


def test_no_data_folder_skips_with_a_notice_and_exit_3(world):
    _, _, _, env = world
    env = {k: v for k, v in env.items() if k != 'SFCOP_ORACLE_DATA'}
    env['SFCOP_ORACLE_CONFIG'] = '/nonexistent/oracle-data'
    r = run_preset(world, 'macos-arm64', env=env)
    assert r.returncode == 3
    assert r.stdout.startswith('skip: macos-arm64: no prepared data folder')


def test_no_client_skips_windows_with_the_reason(world):
    r = run_preset(world, 'windows-x64')
    assert r.returncode == 3
    assert r.stdout.startswith('skip: windows-x64: no lifted client')
    assert 'does not build for windows-x64 yet' in r.stdout


def test_a_failing_client_is_exit_1_and_writes_no_trace(world):
    tmp, build, _, _ = world
    stand_in(build / 'sfcop', 'echo "[fault] invented" >&2\nexit 9\n')
    r = run_preset(world, 'macos-arm64')
    assert r.returncode == 1
    assert 'exited 9' in r.stderr
    assert not (tmp / 'traces' / 'macos-arm64' / 'MenuPath' / 'menu.trace').exists()


def test_a_client_that_reached_no_mark_is_exit_1(world):
    tmp, build, _, _ = world
    stand_in(build / 'sfcop', 'exit 0\n')
    assert run_preset(world, 'macos-arm64').returncode == 1


def test_bad_arguments_are_exit_2(world):
    assert run_preset(world, 'amiga').returncode == 2
    assert run_preset(world, 'macos-arm64', '--bogus').returncode == 2
    assert run_preset(world, 'macos-arm64', '--mission', 'Other', '--input', 'x').returncode == 2


def test_the_linux_preset_goes_through_its_emulator(world):
    tmp, _, _, env = world
    build = tmp / 'builds' / 'linux-x64'
    (build / 'gameroot').mkdir(parents=True)
    stand_in(build / 'sfcop', 'exit 1\n')   # would fail if run directly
    emu = tmp / 'emu.sh'
    stand_in(emu, 'shift\necho "[mark] reached 0x00401000 (WinMain) at 1.0 s" >&2\n')
    env = dict(env, SFCOP_ORACLE_EMULATOR=str(emu))
    r = run_preset(world, 'linux-x64', env=env)
    assert r.returncode == 0, r.stderr
    header = (tmp / 'traces' / 'linux-x64' / 'MenuPath' / 'menu.trace').read_text().split('\n')
    assert header[4].startswith('build\tnative linux-x64 ')


def test_skirmish_trace_is_made_from_the_mission_log_the_client_writes(world):
    tmp, build, _, _ = world
    log = (FIXTURES / 'oracle_log.txt').read_bytes()
    shutil.copy(FIXTURES / 'oracle_log.txt', tmp / 'mission-log.txt')
    stand_in(build / 'sfcop',
             'for a in "$@"; do [ "$a" = --help ] && { echo "  --logs DIR"; exit 0; }; done\n'
             'while [ $# -gt 0 ]; do [ "$1" = --logs ] && l=$2; shift; done\n'
             'cp "' + str(tmp / 'mission-log.txt') + '" "$l/OracleTrace.txt"\n')
    assert log  # the invented two-ship run
    mod = tmp / 'modules' / 'macos-arm64' / 'OracleSkirmish'
    mod.mkdir(parents=True)
    (mod / 'module.json').write_text('{}\n')
    env = dict(world[3], SFCOP_ORACLE_MODULES=str(tmp / 'modules'))
    r = run_preset(world, 'macos-arm64', '--mission', 'OracleSkirmish', '--input', 'skirmish', env=env)
    assert r.returncode == 0, r.stderr
    got = (tmp / 'traces' / 'macos-arm64' / 'OracleSkirmish' / 'skirmish.trace').read_text().split('\n')
    want = (FIXTURES / 'oracle_log.trace').read_text().split('\n')
    assert got[:3] == want[:3]
    assert got[6:] == want[6:]


# --- the comparison -----------------------------------------------------------------------------

def compare_run(*args):
    return subprocess.run([sys.executable, '-I', str(COMPARE), *map(str, args)],
                          capture_output=True, text=True)


def place(root, preset, fixture):
    d = root / preset / 'OracleSkirmish'
    d.mkdir(parents=True, exist_ok=True)
    shutil.copy(FIXTURES / fixture, d / 'skirmish.trace')


THREE = ['macos-arm64', 'linux-x64', 'windows-x64']


def test_three_equal_traces_pass(tmp_path):
    for p in THREE:
        place(tmp_path, p, 'expected.trace' if p == THREE[0] else 'actual_same.trace')
    r = compare_run(tmp_path, *THREE)
    assert r.returncode == 0, r.stdout
    assert 'macos-arm64 vs linux-x64: identical: 6 ticks' in r.stdout
    assert 'linux-x64 vs windows-x64: identical: 6 ticks' in r.stdout
    digests = {line.split()[2] for line in r.stdout.splitlines() if line.startswith('ticks-sha256')}
    assert len(digests) == 1


def test_a_tick_3_difference_fails_with_exit_4_naming_the_tick(tmp_path):
    place(tmp_path, 'macos-arm64', 'expected.trace')
    place(tmp_path, 'linux-x64', 'actual_tick3.trace')
    r = compare_run(tmp_path, 'macos-arm64', 'linux-x64')
    assert r.returncode == 4
    assert 'macos-arm64 vs linux-x64: first difference at tick 3' in r.stdout
    digests = {line.split()[2] for line in r.stdout.splitlines() if line.startswith('ticks-sha256')}
    assert len(digests) == 2


def test_a_missing_trace_fails_with_exit_2(tmp_path):
    place(tmp_path, 'macos-arm64', 'expected.trace')
    r = compare_run(tmp_path, 'macos-arm64', 'linux-x64')
    assert r.returncode == 2
    assert 'linux-x64' in r.stdout


def test_a_truncated_trace_fails_with_exit_2(tmp_path):
    place(tmp_path, 'macos-arm64', 'expected.trace')
    place(tmp_path, 'linux-x64', 'expected.trace')
    path = tmp_path / 'linux-x64' / 'OracleSkirmish' / 'skirmish.trace'
    path.write_bytes(path.read_bytes()[:-20])      # cut in the middle of the last line
    r = compare_run(tmp_path, 'macos-arm64', 'linux-x64')
    assert r.returncode == 2
    assert 'no LF' in r.stdout


def test_a_differing_header_field_is_exit_2(tmp_path):
    place(tmp_path, 'macos-arm64', 'expected.trace')
    place(tmp_path, 'linux-x64', 'actual_header_input.trace')
    assert compare_run(tmp_path, 'macos-arm64', 'linux-x64').returncode == 2


def test_the_expected_tree_is_compared_when_given(tmp_path):
    exp = tmp_path / 'expected' / 'OracleSkirmish'
    exp.mkdir(parents=True)
    shutil.copy(FIXTURES / 'expected.trace', exp / 'skirmish.trace')
    place(tmp_path / 'out', 'macos-arm64', 'actual_same.trace')
    place(tmp_path / 'out', 'linux-x64', 'actual_tick3.trace')
    r = compare_run(tmp_path / 'out', 'macos-arm64', 'linux-x64', '--expected', tmp_path / 'expected')
    assert r.returncode == 4
    assert 'macos-arm64 vs expected: OracleSkirmish/skirmish.trace: identical: 6 ticks' in r.stdout
    assert 'linux-x64 vs expected: OracleSkirmish/skirmish.trace: first difference at tick 3' in r.stdout


def test_an_empty_expected_folder_adds_nothing(tmp_path):
    (tmp_path / 'expected').mkdir()
    place(tmp_path / 'out', 'macos-arm64', 'expected.trace')
    place(tmp_path / 'out', 'linux-x64', 'actual_same.trace')
    r = compare_run(tmp_path / 'out', 'macos-arm64', 'linux-x64', '--expected', tmp_path / 'expected')
    assert r.returncode == 0


def test_the_menu_path_compares_with_the_same_tool(world):
    tmp, build, _, _ = world
    assert run_preset(world, 'macos-arm64').returncode == 0
    shutil.copytree(tmp / 'traces' / 'macos-arm64', tmp / 'traces' / 'linux-x64')
    r = compare_run(tmp / 'traces', 'macos-arm64', 'linux-x64', '--mission', 'MenuPath', '--input', 'menu')
    assert r.returncode == 0, r.stdout
    assert 'identical: 4 ticks' in r.stdout


# --- the runner: modules, niceness, filtered output --------------------------------------------

def test_the_failure_path_drops_lines_naming_the_data_folder(world):
    tmp, build, data, _ = world
    stand_in(build / 'sfcop', 'echo "[fault] opened ' + str(data) + '/registry.tsv" >&2\n'
                              'echo "[fault] harmless line" >&2\nexit 9\n')
    r = run_preset(world, 'macos-arm64')
    assert r.returncode == 1
    assert 'harmless line' in r.stderr
    assert str(data) not in r.stdout + r.stderr


def test_the_skirmish_skips_without_a_module_for_the_preset(world):
    tmp, build, _, env = world
    r = run_preset(world, 'macos-arm64', '--mission', 'OracleSkirmish', '--input', 'skirmish')
    assert r.returncode == 3
    assert 'no OracleSkirmish module for this preset' in r.stdout
    # A module of another preset's ABI is never used.
    other = tmp / 'modules' / 'linux-x64' / 'OracleSkirmish'
    other.mkdir(parents=True)
    (other / 'module.json').write_text('{}\n')
    env = dict(env, SFCOP_ORACLE_MODULES=str(tmp / 'modules'))
    r = run_preset(world, 'macos-arm64', '--mission', 'OracleSkirmish', '--input', 'skirmish', env=env)
    assert r.returncode == 3 and 'no OracleSkirmish module' in r.stdout


def test_the_skirmish_skips_with_a_client_that_has_no_logs_flag(world):
    tmp, build, _, env = world
    mod = tmp / 'modules' / 'macos-arm64' / 'OracleSkirmish'
    mod.mkdir(parents=True)
    (mod / 'module.json').write_text('{}\n')
    env = dict(env, SFCOP_ORACLE_MODULES=str(tmp / 'modules'))
    r = run_preset(world, 'macos-arm64', '--mission', 'OracleSkirmish', '--input', 'skirmish', env=env)
    assert r.returncode == 3
    assert 'no --logs flag' in r.stdout


def test_the_client_gets_the_presets_module_root(world):
    tmp, build, _, env = world
    (tmp / 'modules' / 'macos-arm64').mkdir(parents=True)
    stand_in(build / 'sfcop',
             'echo "[mark] reached 0x00401000 (WinMain) at 1.0 s" >&2\n'
             '[ "$SFCOP_MISSION_MODULES" = "' + str(tmp / 'modules' / 'macos-arm64') + '" ] || exit 7\n')
    env = dict(env, SFCOP_ORACLE_MODULES=str(tmp / 'modules'))
    assert run_preset(world, 'macos-arm64', env=env).returncode == 0


def test_the_linux_container_runs_the_client_through_a_nice_wrapper(world):
    tmp, _, _, env = world
    build = tmp / 'builds' / 'linux-x64'
    (build / 'gameroot').mkdir(parents=True)
    stand_in(build / 'sfcop', 'exit 0\n')
    bindir = tmp / 'bin'
    bindir.mkdir()
    stand_in(bindir / 'docker',
             'echo "$@" > "' + str(tmp / 'docker-args') + '"\n'
             'echo "[mark] reached 0x00401000 (WinMain) at 1.0 s" >&2\n')
    env = dict(env, PATH=str(bindir) + os.pathsep + env['PATH'])
    r = run_preset(world, 'linux-x64', env=env)
    assert r.returncode == 0, r.stderr
    args = (tmp / 'docker-args').read_text()
    assert f'{build}/nice-exec.sh {build}/sfcop --install' in args
    assert (build / 'nice-exec.sh').read_text().endswith('exec nice -n 19 "$@"\n')


# --- the workflow's logic -----------------------------------------------------------------------

sys.path.insert(0, str(PRESETS))
import ci  # noqa: E402


@pytest.fixture
def unpaused(monkeypatch):
    """No preset paused: the logic that applies again once PAUSED_PRESETS is emptied."""
    monkeypatch.setattr(ci, 'PAUSED_PRESETS', ())


def test_the_cross_presets_are_paused():
    assert ci.PAUSED_PRESETS == ('windows-x64', 'linux-x64')


def test_a_paused_preset_is_not_built_for_a_src_change_and_macos_is():
    for files in (['src/app/main.cpp'], ['game/unsorted/tSphere.cpp', 'src/app/main.cpp'], []):
        assert ci.builds('macos-arm64', files), files
        assert not ci.builds('windows-x64', files), files
        assert not ci.builds('linux-x64', files), files


def test_builds_names_the_pause_for_a_paused_preset(monkeypatch, capsys):
    import io
    monkeypatch.setattr('sys.stdin', io.StringIO('src/app/main.cpp\n'))
    assert ci.main(['builds', 'windows-x64']) == 0
    out = capsys.readouterr()
    assert out.out == 'no\n'
    assert out.err == ('::notice::skip: windows-x64: paused on pull requests (PAUSED_PRESETS in '
                       'oracle/presets/ci.py), whatever the change\n')
    monkeypatch.setattr('sys.stdin', io.StringIO('src/app/main.cpp\n'))
    assert ci.main(['builds', 'macos-arm64']) == 0
    out = capsys.readouterr()
    assert out.out == 'yes\n' and out.err == ''


def test_run_skips_a_paused_preset_even_when_its_build_folder_exists(tmp_path, monkeypatch, capsys):
    monkeypatch.setenv('SFCOP_ORACLE_RUN_PRESET', str(fake_run_preset(tmp_path, {
        p: (0, 'wrote') for p in ci.PRESETS})))
    for preset in ci.PRESETS:
        (tmp_path / preset / 'oracle-scratch').mkdir(parents=True)
    changed = tmp_path / 'changed.txt'
    changed.write_text('src/app/main.cpp\n')
    env = tmp_path / 'env'
    assert ci.main(['run', 'MenuPath', 'menu', '--build-root', str(tmp_path), '--traces', str(tmp_path),
                    '--env-file', str(env), '--changed', str(changed)]) == 0
    out = capsys.readouterr().out
    assert env.read_text() == 'RAN_MENUPATH=macos-arm64\n'
    assert 'oracle: macos-arm64: MenuPath/menu: exit 0 (ran)' in out
    assert 'oracle: linux-x64' not in out and 'oracle: windows-x64' not in out
    assert ('::notice::oracle MenuPath/menu: linux-x64 is paused on pull requests '
            '(PAUSED_PRESETS in oracle/presets/ci.py), so its client is not run') in out
    assert ('::notice::oracle MenuPath/menu: windows-x64 is paused on pull requests '
            '(PAUSED_PRESETS in oracle/presets/ci.py), so its client is not run') in out
    assert 'was not built for this pull request' not in out
    # The paused presets' folders are left as they were.
    assert (tmp_path / 'linux-x64' / 'oracle-scratch').is_dir()
    assert (tmp_path / 'windows-x64' / 'oracle-scratch').is_dir()


def test_run_all_without_changed_paths_skips_a_paused_preset(tmp_path, monkeypatch):
    monkeypatch.setenv('SFCOP_ORACLE_RUN_PRESET', str(fake_run_preset(tmp_path, {
        p: (0, 'wrote') for p in ci.PRESETS})))
    ran, failed, log = ci.run_all('MenuPath', 'menu', tmp_path, tmp_path)
    assert ran == ['macos-arm64'] and not failed


def test_a_decompilation_only_change_builds_macos_alone(unpaused):
    grind = ['game/unsorted/tSphere.cpp', 'game/manifest.tsv', 'analysis/results/matched_set.json',
             'src/app/guest/override.cpp']
    assert ci.builds('macos-arm64', grind)
    assert not ci.builds('linux-x64', grind)
    assert not ci.builds('windows-x64', grind)


def test_any_other_change_builds_every_preset(unpaused):
    for files in (['src/app/main.cpp'], ['game/Tiger/tsay.cpp', 'cmake/SfcopLifter.cmake'],
                  ['src/app/guest/override.h'], ['docs/DESIGN.md'], [],
                  ['game/port/types.h'], ['game/unsorted/tSphere.cpp', 'game/port/guest.h'],
                  ['game/UNITS.tsv'], ['analysis/results/retail_hashes.sha256'],
                  ['analysis/results/progress/progress.json']):
        for preset in ('macos-arm64', 'linux-x64', 'windows-x64'):
            assert ci.builds(preset, files), (preset, files)


def test_builds_reads_paths_on_standard_input(unpaused, monkeypatch, capsys):
    import io
    monkeypatch.setattr('sys.stdin', io.StringIO('game/unsorted/tSphere.cpp\n'))
    assert ci.main(['builds', 'linux-x64']) == 0
    assert capsys.readouterr().out == 'no\n'


def test_path_gate_on_a_list_of_files():
    assert ci.path_gate(['README.md', 'src/app/main.cpp'])
    assert ci.path_gate(['game/Tiger/tsay.cpp'])
    assert ci.path_gate(['oracle/presets/run_preset.sh'])
    assert ci.path_gate(['.github/workflows/pull-request.yml'])
    assert not ci.path_gate(['docs/DESIGN.md', 'tests/test_x.py', 'oracle/expected/README.md',
                             '.github/workflows/other.yml', 'srcx/a.cpp'])
    assert not ci.path_gate([])


def test_exit_mapping():
    assert ci.classify(0) == 'ran'
    assert ci.classify(3) == 'skip'
    assert [ci.classify(n) for n in (1, 2, 4, 127)] == ['fail'] * 4


def fake_run_preset(tmp, codes):
    script = tmp / 'fake_run.sh'
    lines = ['preset=$1', 'case $preset in']
    for preset, (code, text) in codes.items():
        lines.append(f'  {preset}) echo "{text}"; exit {code} ;;')
    lines.append('esac')
    script.write_text('\n'.join(lines) + '\n')
    return script


def test_run_all_keeps_the_presets_that_ran_warns_for_skips_and_fails_on_the_rest(unpaused, tmp_path, monkeypatch):
    monkeypatch.setenv('SFCOP_ORACLE_RUN_PRESET', str(fake_run_preset(tmp_path, {
        'macos-arm64': (0, 'wrote'), 'linux-x64': (3, 'skip: linux-x64: no lifted client'),
        'windows-x64': (1, 'broke')})))
    ran, failed, log = ci.run_all('MenuPath', 'menu', tmp_path, tmp_path)
    assert ran == ['macos-arm64'] and failed
    text = '\n'.join(log)
    assert '::warning::oracle MenuPath/menu: linux-x64 did not run: linux-x64: no lifted client' in text
    assert '::error::oracle MenuPath/menu: windows-x64 failed (exit 1)' in text


def test_run_leaves_out_the_presets_a_decompilation_only_change_did_not_build(unpaused, tmp_path, monkeypatch, capsys):
    monkeypatch.setenv('SFCOP_ORACLE_RUN_PRESET', str(fake_run_preset(tmp_path, {
        p: (0, 'wrote') for p in ci.PRESETS})))
    changed = tmp_path / 'changed.txt'
    changed.write_text('game/unsorted/tSphere.cpp\nanalysis/results/matched_set.json\n')
    env = tmp_path / 'env'
    assert ci.main(['run', 'MenuPath', 'menu', '--build-root', str(tmp_path), '--traces', str(tmp_path),
                    '--env-file', str(env), '--changed', str(changed)]) == 0
    out = capsys.readouterr().out
    assert env.read_text() == 'RAN_MENUPATH=macos-arm64\n'
    assert 'oracle: linux-x64' not in out and 'oracle: windows-x64' not in out
    assert '::notice::oracle MenuPath/menu: linux-x64 was not built for this pull request' in out
    assert '::notice::oracle MenuPath/menu: windows-x64 was not built for this pull request' in out


def test_run_runs_every_preset_for_any_other_change(unpaused, tmp_path, monkeypatch, capsys):
    monkeypatch.setenv('SFCOP_ORACLE_RUN_PRESET', str(fake_run_preset(tmp_path, {
        p: (0, 'wrote') for p in ci.PRESETS})))
    changed = tmp_path / 'changed.txt'
    changed.write_text('game/unsorted/tSphere.cpp\nsrc/app/main.cpp\n')
    env = tmp_path / 'env'
    assert ci.main(['run', 'MenuPath', 'menu', '--build-root', str(tmp_path), '--traces', str(tmp_path),
                    '--env-file', str(env), '--changed', str(changed)]) == 0
    assert env.read_text() == 'RAN_MENUPATH=macos-arm64 linux-x64 windows-x64\n'
    assert '::notice::' not in capsys.readouterr().out


def test_run_all_with_only_skips_does_not_fail(unpaused, tmp_path, monkeypatch):
    monkeypatch.setenv('SFCOP_ORACLE_RUN_PRESET', str(fake_run_preset(tmp_path, {
        p: (3, f'skip: {p}: no data') for p in ci.PRESETS})))
    ran, failed, log = ci.run_all('MenuPath', 'menu', tmp_path, tmp_path)
    assert ran == [] and not failed
    assert sum('::warning::' in line for line in log) == 3


def test_run_all_removes_a_scratch_copy_left_by_a_killed_run(unpaused, tmp_path, monkeypatch):
    monkeypatch.setenv('SFCOP_ORACLE_RUN_PRESET', str(fake_run_preset(tmp_path, {
        p: (3, 'skip: x') for p in ci.PRESETS})))
    left = tmp_path / 'linux-x64' / 'oracle-scratch' / 'linux-x64'
    left.mkdir(parents=True)
    (left / 'file').write_text('x')
    ci.run_all('MenuPath', 'menu', tmp_path, tmp_path)
    assert not (tmp_path / 'linux-x64' / 'oracle-scratch').exists()


def test_compare_warns_when_fewer_than_two_presets_ran(tmp_path, capsys):
    place(tmp_path, 'macos-arm64', 'expected.trace')
    assert ci.main(['compare', 'OracleSkirmish', 'skirmish', '--traces', str(tmp_path),
                    '--ran', 'macos-arm64']) == 0
    assert '::warning::oracle OracleSkirmish/skirmish: only macos-arm64 ran, so no pairwise comparison happened' \
        in capsys.readouterr().out
    assert ci.main(['compare', 'OracleSkirmish', 'skirmish', '--traces', str(tmp_path), '--ran', '']) == 0
    assert 'no preset ran' in capsys.readouterr().out


def test_compare_passes_the_expected_folder_and_an_empty_one_is_fine(tmp_path, capfd):
    place(tmp_path / 'out', 'macos-arm64', 'expected.trace')
    place(tmp_path / 'out', 'linux-x64', 'actual_same.trace')
    (tmp_path / 'expected').mkdir()
    assert ci.main(['compare', 'OracleSkirmish', 'skirmish', '--traces', str(tmp_path / 'out'),
                    '--ran', 'macos-arm64 linux-x64', '--expected', str(tmp_path / 'expected')]) == 0
    out = capfd.readouterr().out
    assert 'no expected traces under ' + str(tmp_path / 'expected') in out


def test_the_expected_tree_compare_is_limited_to_the_mission_under_test(tmp_path):
    exp = tmp_path / 'expected' / 'OracleSkirmish'
    exp.mkdir(parents=True)
    shutil.copy(FIXTURES / 'expected.trace', exp / 'skirmish.trace')
    # A menu-path run is not asked for the skirmish's recorded trace.
    for preset in ('macos-arm64', 'linux-x64'):
        d = tmp_path / 'out' / preset / 'MenuPath'
        d.mkdir(parents=True)
        shutil.copy(FIXTURES / 'expected.trace', d / 'menu.trace')
        text = (d / 'menu.trace').read_text().replace('OracleSkirmish', 'MenuPath').replace('skirmish', 'menu')
        (d / 'menu.trace').write_text(text)
    r = compare_run(tmp_path / 'out', 'macos-arm64', 'linux-x64', '--mission', 'MenuPath',
                    '--input', 'menu', '--expected', tmp_path / 'expected')
    assert r.returncode == 0, r.stdout
