"""scripts/check.sh holds the matched set to the base branch's copy.

Each test builds a throwaway repository with this checkout's check.sh, harness/ and scripts/, a
base commit on `develop` (also recorded as origin/develop) and a work branch on top, then runs
check.sh there. The fixture has no install, no lift output and no native build, so those steps
skip; the matched-set scores are still compared with the base.
"""
import json
import os
import shutil
import subprocess

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ENTRY = {'name': 'tAllocator::Get', 'score': 100.0, 'exact': True, 'unit': 'Common/Allocator.cpp',
         'recorded': '2026-01-02'}


def git(repo, *args):
    env = dict(os.environ, GIT_AUTHOR_NAME='TestCaptain', GIT_AUTHOR_EMAIL='captain@example.invalid',
               GIT_COMMITTER_NAME='TestCaptain', GIT_COMMITTER_EMAIL='captain@example.invalid')
    subprocess.run(['git', '-c', 'commit.gpgsign=false', '-c', 'core.hooksPath=/dev/null', *args],
                   cwd=repo, env=env, check=True, capture_output=True)


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w') as f:
        f.write(text)


def matched_set(functions):
    return json.dumps({'version': 1, 'functions': functions}, indent=1) + '\n'


@pytest.fixture
def repo(tmp_path):
    """A repository on branch wp/test whose develop base has one exact function at 100%."""
    repo = str(tmp_path / 'repo')
    os.makedirs(repo)
    # harness/tests test this checkout, not the fixture: a placeholder stands in for them.
    ignore = shutil.ignore_patterns('__pycache__', '*.pyc', 'tests')
    shutil.copytree(os.path.join(ROOT, 'harness'), os.path.join(repo, 'harness'), ignore=ignore)
    write(os.path.join(repo, 'harness', 'tests', 'test_harness_placeholder.py'),
          'def test_harness_placeholder():\n    pass\n')
    for rel in ('scripts/check.sh', 'scripts/port_footprint.py', 'scripts/progress.py',
                '.github/hotspots.txt', '.github/CODEOWNERS'):
        os.makedirs(os.path.dirname(os.path.join(repo, rel)), exist_ok=True)
        shutil.copy(os.path.join(ROOT, rel), os.path.join(repo, rel))
    write(os.path.join(repo, 'tests', 'test_nothing.py'), 'def test_nothing():\n    pass\n')
    write(os.path.join(repo, 'analysis', 'results', 'matched_set.json'),
          matched_set({'0x006fc690': ENTRY}))
    git(repo, 'init', '-q', '-b', 'develop')
    git(repo, 'add', '-A')
    git(repo, 'commit', '-q', '-m', 'base')
    git(repo, 'update-ref', 'refs/remotes/origin/develop', 'HEAD')
    git(repo, 'checkout', '-q', '-b', 'wp/test')
    return repo


def commit_matched_set(repo, functions):
    write(os.path.join(repo, 'analysis', 'results', 'matched_set.json'), matched_set(functions))
    git(repo, 'commit', '-q', '-am', 'change the matched set')


def check(repo, tmp_path):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('SFCOP_', 'HARNESS_', 'GITHUB_'))}
    work = str(tmp_path / 'work')
    os.makedirs(work, exist_ok=True)
    env.update(SFCOP_WORK=work, SFCOP_REFERENCE=str(tmp_path / 'no-install'),
               HARNESS_STATE=str(tmp_path / 'state'))
    return subprocess.run(['sh', 'scripts/check.sh'], cwd=repo, env=env, capture_output=True,
                          text=True)


def test_a_lowered_score_fails_the_check(repo, tmp_path):
    commit_matched_set(repo, {'0x006fc690': dict(ENTRY, score=97.5, exact=False)})
    p = check(repo, tmp_path)
    assert p.returncode != 0, p.stdout
    assert 'REGRESSION 0x006fc690 tAllocator::Get: matched_set.json lowers to 97.50%' in p.stdout
    assert 'check: ok' not in p.stdout


def test_a_dropped_entry_fails_the_check(repo, tmp_path):
    commit_matched_set(repo, {})
    p = check(repo, tmp_path)
    assert p.returncode != 0, p.stdout
    assert 'REGRESSION 0x006fc690 tAllocator::Get: matched_set.json drops the entry' in p.stdout


def test_an_unchanged_or_added_entry_passes_and_says_what_it_skipped(repo, tmp_path):
    commit_matched_set(repo, {'0x006fc690': ENTRY,
                              '0x006fc6f0': dict(ENTRY, name='tAllocator::Put', score=88.0, exact=False)})
    p = check(repo, tmp_path)
    assert p.returncode == 0, p.stdout + p.stderr
    out = p.stdout.splitlines()
    assert 'skip: no game/ tree yet' in out
    assert 'skip: no lifter output (build the native client, or set SFCOP_LIFTED)' in out
    assert 'skip: no recorded trace in oracle/expected/' in out
    assert any(line.startswith('ok: 1 entries held to ') for line in out)
    assert out[-1] == 'check: ok'
