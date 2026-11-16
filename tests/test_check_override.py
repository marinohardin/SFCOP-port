"""scripts/check.sh holds src/app/guest/override.cpp to the decompilation's annotations.

Each test builds a throwaway repository with this checkout's check.sh and the hotspot files, and,
for the decompilation, a folder with an INTERFACE_VERSION and a stand-in tools/manifest.py: a
script that records the arguments it was given and exits $STANDIN_EXIT, where the real one exits 1
when the table is not what the annotations give. The fixture has no native build and no recorded
trace, so those steps skip.
"""
import os
import shutil
import stat
import subprocess

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

STANDIN = '''#!/usr/bin/env python3
# A stand-in for the decompilation's tools/manifest.py (tests/test_check_override.py).
import os
import sys
with open(os.environ['STANDIN_ARGS'], 'w') as f:
    f.write('\\n'.join(sys.argv[1:]) + '\\n')
sys.exit(int(os.environ.get('STANDIN_EXIT', '0')))
'''


def write(path, text, executable=False):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write(text)
    if executable:
        os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)


def decomp_dir(path, version='1\n'):
    """A stand-in decompilation folder at `path`."""
    if version is not None:
        write(os.path.join(path, 'INTERFACE_VERSION'), version)
    write(os.path.join(path, 'tools', 'manifest.py'), STANDIN, executable=True)
    return path


@pytest.fixture
def repo(tmp_path):
    """A repository with check.sh, the hotspot files and one passing test, and no decomp/."""
    repo = str(tmp_path / 'repo')
    for rel in ('scripts/check.sh', '.github/hotspots.txt', '.github/CODEOWNERS'):
        os.makedirs(os.path.dirname(os.path.join(repo, rel)), exist_ok=True)
        shutil.copy(os.path.join(ROOT, rel), os.path.join(repo, rel))
    write(os.path.join(repo, 'tests', 'test_nothing.py'), 'def test_nothing():\n    pass\n')
    write(os.path.join(repo, 'src', 'app', 'guest', 'override.cpp'), '// a table\n')
    return repo


def check(repo, tmp_path, standin_exit=0, decomp=None):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('SFCOP_', 'HARNESS_', 'GITHUB_', 'STANDIN_'))}
    env.update(STANDIN_EXIT=str(standin_exit), STANDIN_ARGS=str(tmp_path / 'standin-args.txt'),
               SFCOP_ORACLE_CONFIG=str(tmp_path / 'no-such-config'))
    if decomp is not None:
        env['SFCOP_DECOMP_DIR'] = decomp
    return subprocess.run(['sh', 'scripts/check.sh'], cwd=repo, env=env, capture_output=True,
                          text=True)


def standin_args(tmp_path):
    with open(tmp_path / 'standin-args.txt') as f:
        return f.read().splitlines()


def test_without_the_submodule_the_step_skips_and_says_why(repo, tmp_path):
    p = check(repo, tmp_path)
    assert p.returncode == 0, p.stdout + p.stderr
    out = p.stdout.splitlines()
    assert 'skip: no decompilation at %s/decomp (git submodule update --init)' % repo in out
    assert 'skip: no recorded trace in oracle/expected/' in out
    assert out[-1] == 'check: ok'
    assert not (tmp_path / 'standin-args.txt').exists()


def test_a_submodule_without_its_interface_version_is_skipped(repo, tmp_path):
    decomp_dir(os.path.join(repo, 'decomp'), version=None)
    p = check(repo, tmp_path)
    assert p.returncode == 0, p.stdout + p.stderr
    assert 'skip: no decompilation at %s/decomp (git submodule update --init)' % repo \
        in p.stdout.splitlines()
    assert not (tmp_path / 'standin-args.txt').exists()


def test_a_current_table_passes_and_is_checked_with_the_interface_command(repo, tmp_path):
    decomp_dir(os.path.join(repo, 'decomp'))
    p = check(repo, tmp_path)
    assert p.returncode == 0, p.stdout + p.stderr
    out = p.stdout.splitlines()
    assert ('ok: src/app/guest/override.cpp is what %s/decomp/tools/manifest.py writes' % repo) in out
    assert out[-1] == 'check: ok'
    assert standin_args(tmp_path) == ['--only-override', '--override',
                                      'src/app/guest/override.cpp', '--check']


def test_a_stale_table_fails_the_check(repo, tmp_path):
    decomp_dir(os.path.join(repo, 'decomp'))
    p = check(repo, tmp_path, standin_exit=1)
    assert p.returncode == 1, p.stdout + p.stderr
    assert 'check: ok' not in p.stdout


def test_sfcop_decomp_dir_names_a_decompilation_outside_the_repository(repo, tmp_path):
    elsewhere = decomp_dir(str(tmp_path / 'elsewhere'))
    p = check(repo, tmp_path, decomp=elsewhere)
    assert p.returncode == 0, p.stdout + p.stderr
    assert ('ok: src/app/guest/override.cpp is what %s/tools/manifest.py writes' % elsewhere) \
        in p.stdout.splitlines()
