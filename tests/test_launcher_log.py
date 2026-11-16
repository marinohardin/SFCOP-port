"""packaging/macos/launcher.sh gives each run two log files, launch-<stamp>.log (everything) and
important-<stamp>.log (the lines the client marks important), and keeps the newest ten runs.

The client is a stand-in script beside a copy of the launcher: it prints a detail line to stderr
and an important line to stderr and to the file SFCOP_IMPORTANT_LOG names, as src/platform/Core/Log.h
does; stderr is a pipe so the launcher takes its no-terminal path. Nothing runs the game."""
import os
import pathlib
import re
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
LAUNCHER = ROOT / 'packaging/macos/launcher.sh'

CLIENT = '''#!/bin/sh
echo "[guest] App could not find 'X' to do connection" >&2
echo "[scripts] refused: m01" >&2
[ -n "$SFCOP_IMPORTANT_LOG" ] && echo "[scripts] refused: m01" >> "$SFCOP_IMPORTANT_LOG"
exit 0
'''


def run_launcher(tmp_path, data, env_extra=None):
    bundle = tmp_path / 'MacOS'
    bundle.mkdir(exist_ok=True)
    shutil.copy(LAUNCHER, bundle / 'sfcop-launch')
    client = bundle / 'sfcop'
    client.write_text(CLIENT)
    client.chmod(0o755)
    env = dict(os.environ)
    env.pop('SFCOP_LAUNCH_LOG', None)
    env.update(env_extra or {})
    subprocess.run(['sh', str(bundle / 'sfcop-launch'), '--data', str(data)], env=env,
                   stderr=subprocess.PIPE, check=True)


def names(data, prefix):
    return sorted(p.name for p in (data / 'logs').glob(prefix + '-*.log'))


def test_an_important_line_lands_in_both_files_and_a_debug_line_only_in_the_full_one(tmp_path):
    data = tmp_path / 'data'
    run_launcher(tmp_path, data)
    (full,) = names(data, 'launch')
    (important,) = names(data, 'important')
    stamp = re.fullmatch(r'launch-(\d{8}-\d{6})\.log', full).group(1)
    assert important == 'important-%s.log' % stamp  # one stamp for the pair
    full_text = (data / 'logs' / full).read_text()
    important_text = (data / 'logs' / important).read_text()
    assert "App could not find 'X'" in full_text
    assert '[scripts] refused: m01' in full_text
    assert important_text == '[scripts] refused: m01\n'
    assert not (data / 'logs/launch.log').exists()


def test_eleven_runs_leave_ten_with_both_files_of_each_together(tmp_path):
    data = tmp_path / 'data'
    (data / 'logs').mkdir(parents=True)
    for day in range(1, 11):  # ten earlier runs, then this one is the eleventh
        stamp = '202601%02d-120000' % day
        (data / 'logs' / ('launch-%s.log' % stamp)).write_text('old\n')
        (data / 'logs' / ('important-%s.log' % stamp)).write_text('old\n')
    (data / 'logs/launch.log').write_text('the file from before this change\n')
    run_launcher(tmp_path, data)
    full, important = names(data, 'launch'), names(data, 'important')
    assert len(full) == 10 and len(important) == 10
    assert [n[len('launch-'):] for n in full] == [n[len('important-'):] for n in important]
    assert 'launch-20260101-120000.log' not in full
    assert 'launch-20260102-120000.log' in full
    assert full[-1] > 'launch-20260110-120000.log'  # this run
    assert (data / 'logs/launch.log').read_text().startswith('the file from before')


def test_two_runs_in_one_second_keep_separate_files(tmp_path):
    data = tmp_path / 'data'
    run_launcher(tmp_path, data)
    run_launcher(tmp_path, data)
    assert len(names(data, 'launch')) in (1, 2)  # a second boundary may fall between the runs
    total = sum((data / 'logs' / n).read_text().count('[scripts] refused: m01')
                for n in names(data, 'launch'))
    assert total == 2


def test_a_named_log_file_is_appended_to_and_nothing_rotates(tmp_path):
    data = tmp_path / 'data'
    (data / 'logs').mkdir(parents=True)
    for day in range(1, 13):
        (data / 'logs' / ('launch-202601%02d-120000.log' % day)).write_text('old\n')
    chosen = tmp_path / 'chosen.log'
    for _ in range(2):
        run_launcher(tmp_path, data, {'SFCOP_LAUNCH_LOG': str(chosen)})
    assert chosen.read_text().count('[scripts] refused: m01') == 2
    assert len(names(data, 'launch')) == 12


def test_a_log_dir_from_the_environment_replaces_the_data_folders(tmp_path):
    data = tmp_path / 'data'
    elsewhere = tmp_path / 'elsewhere'
    run_launcher(tmp_path, data, {'SFCOP_LOG_DIR': str(elsewhere)})
    assert len(list(elsewhere.glob('launch-*.log'))) == 1
    assert len(list(elsewhere.glob('important-*.log'))) == 1
    assert not (data / 'logs').exists()
