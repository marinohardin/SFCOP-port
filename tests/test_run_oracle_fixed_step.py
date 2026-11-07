"""oracle/run_oracle.sh with and without ORACLE_FIXED_STEP, on a stand-in Wine.

Each test copies run_oracle.sh, wine-env.sh and normalize_trace.sh into a throwaway oracle/
folder, makes a throwaway SFCOP root holding the built programs (empty stand-ins) and a prefix
holding StarFleetOP.exe, and puts a stand-in `wine` and `wineserver` first on the PATH through
WINE_BIN. The stand-in `wine` records each start (its folder and arguments) and, for the game,
writes a one-sample OracleTrace.txt ending in END, so the run completes at once.
ORACLE_START_WAIT=0 drops the script's wait between the game and the input driver.
"""
import os
import shutil
import stat
import subprocess

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

WINE = '''#!/bin/sh
# A stand-in for wine (tests/test_run_oracle_fixed_step.py).
if [ "$1" = --version ]; then echo wine-standin; exit 0; fi
echo "$(basename "$PWD"): $*" >> "$STANDIN_LOG"
case $1 in
StarFleetOP.exe|oracle_launch.exe)
    printf '# oracle-trace v1\\r\\nS 0 c101 spd 0\\r\\nEND 1 max_samples\\r\\n' > OracleTrace.txt ;;
esac
'''


def _executable(path, text):
    with open(path, 'w') as f:
        f.write(text)
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)


def _setup(tmp_path, launcher=True):
    oracle = tmp_path / 'repo' / 'oracle'
    (oracle / 'driver').mkdir(parents=True)
    for name in ('run_oracle.sh', 'wine-env.sh', 'normalize_trace.sh'):
        shutil.copy(os.path.join(ROOT, 'oracle', name), oracle / name)
    (oracle / 'driver' / 'skirmish.steps').write_text('sleep 1\n')
    root = tmp_path / 'root'
    out = root / 'work' / 'oracle' / 'build' / 'out'
    out.mkdir(parents=True)
    names = ['OracleSkirmish.scr', 'oracle_input.exe']
    if launcher:
        names += ['oracle_launch.exe', 'oracle_clock.dll']
    for name in names:
        (out / name).write_text(name)
    game = tmp_path / 'prefix' / 'drive_c' / 'Games' / 'SFCOP'
    (game / 'Assets' / 'Scripts').mkdir(parents=True)
    (game / 'StarFleetOP.exe').write_text('exe')
    wine_bin = tmp_path / 'winebin'
    wine_bin.mkdir()
    _executable(wine_bin / 'wine', WINE)
    _executable(wine_bin / 'wineserver', '#!/bin/sh\nexit 0\n')
    env = dict(os.environ, SFCOP_ROOT=str(root), WINE_BIN=str(wine_bin),
               WINEPREFIX=str(tmp_path / 'prefix'), STANDIN_LOG=str(tmp_path / 'wine.log'),
               ORACLE_TIMEOUT='20', ORACLE_START_WAIT='0')
    env.pop('ORACLE_FIXED_STEP', None)
    return oracle, game, env


def _run(oracle, env):
    return subprocess.run(['sh', str(oracle / 'run_oracle.sh'), 'label1'], env=env,
                          capture_output=True, text=True, timeout=120)


def _starts(tmp_path):
    return (tmp_path / 'wine.log').read_text().splitlines()


def test_unset_starts_the_game_directly_and_copies_no_launcher(tmp_path):
    oracle, game, env = _setup(tmp_path)
    result = _run(oracle, env)
    assert result.returncode == 0, result.stderr
    assert result.stdout.endswith('  label1  END 1 max_samples\n')
    assert _starts(tmp_path) == ['SFCOP: StarFleetOP.exe', 'SFCOP: oracle_input.exe skirmish.steps']
    assert not (game / 'oracle_launch.exe').exists()
    assert not (game / 'oracle_clock.dll').exists()
    run = (oracle / 'traces' / 'label1' / 'run.txt').read_text().splitlines()
    assert [line.split(' ')[0] for line in run] == ['label', 'date', 'wine', 'exe', 'scr', 'steps']


def test_set_starts_the_game_through_the_launcher_at_that_step(tmp_path):
    oracle, game, env = _setup(tmp_path)
    env['ORACLE_FIXED_STEP'] = '16.667'
    result = _run(oracle, env)
    assert result.returncode == 0, result.stderr
    assert _starts(tmp_path) == [
        'SFCOP: oracle_launch.exe --fixed-step 16.667 StarFleetOP.exe',
        'SFCOP: oracle_input.exe skirmish.steps']
    assert (game / 'oracle_launch.exe').read_text() == 'oracle_launch.exe'
    assert (game / 'oracle_clock.dll').read_text() == 'oracle_clock.dll'
    run = (oracle / 'traces' / 'label1' / 'run.txt').read_text().splitlines()
    assert [line.split(' ')[0] for line in run] == [
        'label', 'date', 'wine', 'exe', 'scr', 'steps', 'fixed_step', 'launch', 'clock']
    assert 'fixed_step 16.667' in run


def test_set_without_the_launcher_refuses_before_starting_anything(tmp_path):
    oracle, game, env = _setup(tmp_path, launcher=False)
    env['ORACLE_FIXED_STEP'] = '16.667'
    result = _run(oracle, env)
    assert result.returncode == 2
    assert 'ORACLE_FIXED_STEP is set but' in result.stderr
    assert 'oracle_launch.exe is missing' in result.stderr
    assert not (tmp_path / 'wine.log').exists()
    assert not (oracle / 'traces').exists()


@pytest.mark.parametrize('step', ['16ms', '0', '0.000', '2000', '1000.001', '1.2345', '.5'])
def test_a_step_out_of_the_launchers_range_is_refused_before_starting_anything(tmp_path, step):
    oracle, game, env = _setup(tmp_path)
    env['ORACLE_FIXED_STEP'] = step
    result = _run(oracle, env)
    assert result.returncode == 2
    assert ('ORACLE_FIXED_STEP is milliseconds above 0 and up to 1000, with at most three '
            "decimals, such as 16.667; not '%s'" % step) in result.stderr
    assert not (tmp_path / 'wine.log').exists()
    assert not (oracle / 'traces').exists()


@pytest.mark.parametrize('step', ['0.001', '1000'])
def test_the_ends_of_the_range_are_accepted(tmp_path, step):
    oracle, game, env = _setup(tmp_path)
    env['ORACLE_FIXED_STEP'] = step
    result = _run(oracle, env)
    assert result.returncode == 0, result.stderr
    assert _starts(tmp_path)[0] == 'SFCOP: oracle_launch.exe --fixed-step %s StarFleetOP.exe' % step
