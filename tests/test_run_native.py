"""oracle/driver/run_native.sh: the arguments it gives the client, with a stand-in client."""
import os
import pathlib
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[1]
RUN = ROOT / 'oracle' / 'driver' / 'run_native.sh'


@pytest.fixture
def world(tmp_path):
    data = tmp_path / 'data'
    data.mkdir()
    client = tmp_path / 'sfcop'
    client.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "' + str(tmp_path / 'args') + '"\n')
    client.chmod(0o755)
    env = {k: v for k, v in os.environ.items() if not k.startswith('SFCOP_')}
    env.update(SFCOP_ORACLE_DATA=str(data), SFCOP_BIN=str(client))
    return tmp_path, env


def run(world, env):
    tmp, _ = world
    r = subprocess.run(['sh', str(RUN), '--headless'], env=env, capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return (tmp / 'args').read_text().splitlines()


def test_the_client_runs_with_the_low_memory_fix_off(world):
    _, env = world
    args = run(world, env)
    assert '--fix=fix_hle_low_memory_reads=off' in args
    assert '--install' not in args


def test_with_an_install_the_client_still_runs_with_the_low_memory_fix_off(world):
    tmp, env = world
    args = run(world, dict(env, SFCOP_INSTALL=str(tmp / 'install')))
    assert '--fix=fix_hle_low_memory_reads=off' in args
    assert args[args.index('--install') + 1] == str(tmp / 'install')
