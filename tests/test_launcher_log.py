"""packaging/macos/launcher.sh writes one log file per run and keeps the newest ten.

The client is a stand-in script beside a copy of the launcher; stderr is a pipe so the launcher takes
its no-terminal path. Nothing runs the game."""
import os
import pathlib
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
LAUNCHER = ROOT / 'packaging/macos/launcher.sh'


def run_launcher(tmp_path, data, env_extra=None):
    bundle = tmp_path / 'MacOS'
    bundle.mkdir(exist_ok=True)
    shutil.copy(LAUNCHER, bundle / 'sfcop-launch')
    client = bundle / 'sfcop'
    client.write_text('#!/bin/sh\necho "client says hi" >&2\n')
    client.chmod(0o755)
    env = dict(os.environ)
    env.pop('SFCOP_LAUNCH_LOG', None)
    env.update(env_extra or {})
    subprocess.run(['sh', str(bundle / 'sfcop-launch'), '--data', str(data)], env=env,
                   stderr=subprocess.PIPE, check=True)


def logs(data):
    return sorted(p.name for p in (data / 'logs').glob('launch-*.log'))


def test_a_run_writes_its_own_timestamped_file_with_the_start_line(tmp_path):
    data = tmp_path / 'data'
    run_launcher(tmp_path, data)
    (name,) = logs(data)
    assert len(name) == len('launch-20260101-000000.log')
    text = (data / 'logs' / name).read_text()
    assert text.startswith('=== SFCOP started ')
    assert 'client says hi' in text
    assert not (data / 'logs/launch.log').exists()


def test_the_newest_ten_files_are_kept_and_older_ones_deleted(tmp_path):
    data = tmp_path / 'data'
    (data / 'logs').mkdir(parents=True)
    for day in range(1, 13):
        (data / 'logs' / ('launch-202601%02d-120000.log' % day)).write_text('old\n')
    (data / 'logs/launch.log').write_text('the file from before this change\n')
    (data / 'logs/other.log').write_text('not ours\n')
    run_launcher(tmp_path, data)
    kept = logs(data)
    assert len(kept) == 10
    assert 'launch-20260101-120000.log' not in kept
    assert 'launch-20260103-120000.log' not in kept
    assert 'launch-20260104-120000.log' in kept
    assert kept[-1] > 'launch-20260112-120000.log'  # this run's own file, the newest
    assert (data / 'logs/launch.log').exists() and (data / 'logs/other.log').exists()


def test_a_named_log_file_is_appended_to_and_nothing_rotates(tmp_path):
    data = tmp_path / 'data'
    (data / 'logs').mkdir(parents=True)
    for day in range(1, 13):
        (data / 'logs' / ('launch-202601%02d-120000.log' % day)).write_text('old\n')
    chosen = tmp_path / 'chosen.log'
    run_launcher(tmp_path, data, {'SFCOP_LAUNCH_LOG': str(chosen)})
    run_launcher(tmp_path, data, {'SFCOP_LAUNCH_LOG': str(chosen)})
    assert chosen.read_text().count('=== SFCOP started') == 2
    assert len(logs(data)) == 12
