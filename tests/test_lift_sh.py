"""cmake/lifter/lift.sh: a lift that follows an interrupted one.

Docker and git are stand-ins on PATH, so nothing is lifted: the fake `docker run` fails, and the
test only checks how far the script gets. No network, no container, no real executable.
"""
import hashlib
import os
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
LIFT = ROOT / 'cmake/lifter/lift.sh'
PINNED = 'ef3f0da6e08a8eb7a3f869273f3df071ca8d7373'


def stub(path, body):
    path.write_text('#!/bin/sh\n' + body)
    path.chmod(0o755)


def test_a_read_only_copy_left_by_an_interrupted_lift_does_not_stop_the_next(tmp_path):
    bin_dir = tmp_path / 'bin'
    bin_dir.mkdir()
    stub(bin_dir / 'docker', 'case "$1" in run) exit 1 ;; *) exit 0 ;; esac\n')
    stub(bin_dir / 'git', f'echo {PINNED}\n')
    pcrecomp = tmp_path / 'pcrecomp'
    (pcrecomp / 'tools/lift').mkdir(parents=True)
    (pcrecomp / 'tools/lift/lift32.py').write_text('')
    exe = tmp_path / 'game.exe'
    exe.write_bytes(b'not a game')
    exe.chmod(0o444)                       # the original is read-only, and so is each copy
    out = tmp_path / 'lift'
    (out / 'game').mkdir(parents=True)
    left = out / 'game/StarFleetOP.exe'    # what a lift stopped after its copy leaves behind
    left.write_bytes(b'older copy')
    left.chmod(0o444)

    env = dict(os.environ, PATH=f'{bin_dir}{os.pathsep}{os.environ["PATH"]}')
    r = subprocess.run(['sh', str(LIFT), '--exe', str(exe),
                        '--sha256', hashlib.sha256(b'not a game').hexdigest(),
                        '--pcrecomp', str(pcrecomp), '--out', str(out), '--files', '4',
                        '--marks', '0x00401000'], env=env, capture_output=True, text=True)

    assert 'Permission denied' not in r.stderr
    assert 'lift: rtti failed' in r.stderr  # it copied the executable and went on to the first step
    assert left.read_bytes() == b'not a game'
