"""scripts/check.sh plays every recorded trace on the native client and compares (the "replay
oracle" block).

Each test builds a throwaway repository with this checkout's check.sh and the trace tools, one
recorded trace (C5's fixture pair under oracle/trace/fixtures/, invented), a steps file for it, and a stand-in client at build/macos-arm64/sfcop: a shell script that records the
arguments it was given, writes the trace named by $STANDIN_TRACE where the real client would
(<logs>/<mission>/<input>.trace) and exits $STANDIN_EXIT. The prepared data folder is an empty
directory named by SFCOP_ORACLE_DATA.
"""
import os
import shutil
import stat
import subprocess

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIXTURES = os.path.join(ROOT, 'oracle', 'trace', 'fixtures')

STANDIN = '''#!/bin/sh
# A stand-in for the native client (tests/test_check_oracle.py).
printf '%s\\n' "$@" > "$STANDIN_ARGS"
logs=; trace=
while [ $# -gt 0 ]; do
    case $1 in
    --logs) logs=$2; shift 2 ;;
    --oracle) trace=$2; shift 2 ;;
    *) shift ;;
    esac
done
mission=$(basename "$(dirname "$trace")")
input=$(basename "$trace" .trace)
mkdir -p "$logs/$mission"
cp "$STANDIN_TRACE" "$logs/$mission/$input.trace"
echo "[oracle] END at 1.0 s"
echo "[oracle] compare.py exit ${STANDIN_EXIT:-0}"
exit ${STANDIN_EXIT:-0}
'''


def git(repo, *args):
    env = dict(os.environ, GIT_AUTHOR_NAME='TestCaptain', GIT_AUTHOR_EMAIL='captain@example.invalid',
               GIT_COMMITTER_NAME='TestCaptain', GIT_COMMITTER_EMAIL='captain@example.invalid')
    subprocess.run(['git', '-c', 'commit.gpgsign=false', '-c', 'core.hooksPath=/dev/null', *args],
                   cwd=repo, env=env, check=True, capture_output=True)


def write(path, text, executable=False):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write(text)
    if executable:
        os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


@pytest.fixture
def repo(tmp_path):
    """A repository on wp/test with one recorded trace, its steps file and the stand-in client."""
    repo = str(tmp_path / 'repo')
    os.makedirs(repo)
    for rel in ('scripts/check.sh', '.github/hotspots.txt', '.github/CODEOWNERS',
                'oracle/trace/compare.py', 'oracle/trace/mklog.py'):
        os.makedirs(os.path.dirname(os.path.join(repo, rel)), exist_ok=True)
        shutil.copy(os.path.join(ROOT, rel), os.path.join(repo, rel))
    write(os.path.join(repo, 'tests', 'test_nothing.py'), 'def test_nothing():\n    pass\n')
    shutil.copy(os.path.join(FIXTURES, 'expected.trace'),
                os.path.join(_mkdir(repo, 'oracle/expected/OracleSkirmish'), 'skirmish.trace'))
    write(os.path.join(repo, 'oracle', 'driver', 'skirmish.steps'),
          '# the stand-in reads no steps\nwindow 60 Starfleet Command\n')
    git(repo, 'init', '-q', '-b', 'develop')
    git(repo, 'add', '-A')
    git(repo, 'commit', '-q', '-m', 'base')
    git(repo, 'update-ref', 'refs/remotes/origin/develop', 'HEAD')
    git(repo, 'checkout', '-q', '-b', 'wp/test')
    # The client and the build directory are not committed, as the real ones are not.
    write(os.path.join(repo, 'build', 'macos-arm64', 'sfcop'), STANDIN, executable=True)
    return repo


def _mkdir(repo, rel):
    path = os.path.join(repo, rel)
    os.makedirs(path, exist_ok=True)
    return path


def check(repo, tmp_path, actual, standin_exit=0, data=True):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('SFCOP_', 'HARNESS_', 'GITHUB_', 'STANDIN_'))}
    work = str(tmp_path / 'work')
    os.makedirs(work, exist_ok=True)
    env.update(SFCOP_WORK=work, SFCOP_REFERENCE=str(tmp_path / 'no-install'),
               HARNESS_STATE=str(tmp_path / 'state'),
               SFCOP_ORACLE_CONFIG=str(tmp_path / 'no-such-config'),
               STANDIN_TRACE=os.path.join(FIXTURES, actual),
               STANDIN_EXIT=str(standin_exit),
               STANDIN_ARGS=str(tmp_path / 'standin-args.txt'))
    if data:
        env['SFCOP_ORACLE_DATA'] = str(_mkdir(str(tmp_path), 'prepared-data'))
    return subprocess.run(['sh', 'scripts/check.sh'], cwd=repo, env=env, capture_output=True,
                          text=True)


def standin_args(tmp_path):
    with open(tmp_path / 'standin-args.txt') as f:
        return f.read().splitlines()


def test_a_matching_trace_passes_and_the_client_is_run_on_the_prepared_folder(repo, tmp_path):
    p = check(repo, tmp_path, 'actual_same.trace')
    assert p.returncode == 0, p.stdout + p.stderr
    out = p.stdout.splitlines()
    assert 'oracle: OracleSkirmish/skirmish on build/macos-arm64/sfcop, headless, up to 900 s' in out
    assert 'OracleSkirmish/skirmish.trace: identical: 6 ticks' in out
    assert out[-1] == 'check: ok'
    args = standin_args(tmp_path)
    assert args[args.index('--data') + 1] == str(tmp_path / 'prepared-data')
    assert args[args.index('--oracle') + 1] == 'oracle/expected/OracleSkirmish/skirmish.trace'
    assert args[args.index('--input') + 1] == 'oracle/driver/skirmish.steps'
    assert args[args.index('--watchdog') + 1] == '900'
    assert '--headless' in args
    logs = args[args.index('--logs') + 1]
    assert not logs.startswith(str(tmp_path / 'prepared-data'))


def test_a_tick_3_difference_fails_with_4_naming_the_tick(repo, tmp_path):
    p = check(repo, tmp_path, 'actual_tick3.trace', standin_exit=4)
    assert p.returncode == 4, p.stdout + p.stderr
    assert ('OracleSkirmish/skirmish.trace: first difference at tick 3: expected '
            + 'a3' * 32 + ', actual ' + 'b3' * 32) in p.stdout.splitlines()
    assert 'check: ok' not in p.stdout


def test_a_client_that_fails_to_play_fails_the_check_with_its_status(repo, tmp_path):
    p = check(repo, tmp_path, 'actual_same.trace', standin_exit=1)
    assert p.returncode == 1, p.stdout + p.stderr
    assert 'oracle: the client exited 1' in p.stdout.splitlines()


def test_without_a_prepared_folder_the_step_skips_and_says_why(repo, tmp_path):
    p = check(repo, tmp_path, 'actual_same.trace', data=False)
    assert p.returncode == 0, p.stdout + p.stderr
    out = p.stdout.splitlines()
    assert ('skip: no prepared data folder with a stored key (set SFCOP_ORACLE_DATA, or put its '
            'path on the first line of $SFCOP_ORACLE_CONFIG)') in out
    assert out[-1] == 'check: ok'
    assert not (tmp_path / 'standin-args.txt').exists()


def test_a_trace_without_a_steps_file_is_bad_input(repo, tmp_path):
    os.remove(os.path.join(repo, 'oracle', 'driver', 'skirmish.steps'))
    p = check(repo, tmp_path, 'actual_same.trace')
    assert p.returncode == 2, p.stdout + p.stderr
    assert ('oracle: oracle/expected/OracleSkirmish/skirmish.trace has no steps file '
            'oracle/driver/skirmish.steps') in p.stdout.splitlines()
