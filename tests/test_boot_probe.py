"""tools/boot_probe.sh runs a boot command N times and writes one classified TSV row per run."""
import os
import re
import stat
import subprocess
import time
import sys

import pytest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROBE = os.path.join(ROOT, 'tools', 'boot_probe.sh')

# A stand-in for the boot: prints the log lines a real run prints for each outcome. The outcome
# for run k is the k-th word of $FAKE_OUTCOMES; the run number comes from a counter file.
FAKE_BOOT = r'''#!/bin/sh
game=$1
counter=$FAKE_DIR/counter
n=$(cat "$counter" 2>/dev/null || echo 0); n=$((n + 1)); echo $n > "$counter"
set -- $FAKE_OUTCOMES
shift $((n - 1))
# The game directory is a fresh copy each run: nothing an earlier run wrote is there.
[ -f "$game/seed.txt" ] || { echo "no seed.txt" >&2; exit 9; }
[ ! -f "$game/written.txt" ] || { echo "saw an earlier run's file" >&2; exit 9; }
echo x > "$game/written.txt"
echo "[pe] mapped" >&2
case $1 in
prompt) echo '[file] C:\assets\models\space\space21.mod' >&2
        echo "[mark] reached 0x00484102 (CD-key prompt) at 5.295 s (last import RegOpenKeyExA)" >&2
        echo "[boot] watchdog after 12 s: the boot reached the CD-key prompt" >&2; exit 0 ;;
fault)  printf '\n=== fault: signal 10, address 0x100000000004 at 0.311 s ===\n' >&2
        echo "  in lifted sub_0042E26B, last import LeaveCriticalSection" >&2; exit 1 ;;
throw)  echo "[throw] RaiseException(0xE06D7363) in lifted sub_00737479" >&2
        echo "[throw] guest C++ exceptions are not handled yet (Wave 1 B6): stopping" >&2; exit 1 ;;
alive)  printf '\n=== watchdog at 12.001 s ===\n' >&2; exit 3 ;;
*)      echo "cannot enter the game directory" >&2; exit 2 ;;
esac
'''

# Prints the epoch seconds it sees through time(), gettimeofday() and clock_gettime(): the fake
# clock shifts the first two, the ones the game's time of day comes from, and leaves the third,
# which libc++ builds the deadlines of timed waits from.
CLOCK_READER = r'''
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
int main(void) {
    struct timespec ts;
    struct timeval tv;
    clock_gettime(CLOCK_REALTIME, &ts);
    gettimeofday(&tv, NULL);
    printf("%ld %ld %ld\n", (long)time(NULL), (long)tv.tv_sec, (long)ts.tv_sec);
    return 0;
}
'''


def clean_env(**extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith('SFCOP_')}
    env.update(extra)
    return env


def setup(tmp_path, outcomes):
    fake = tmp_path / 'fake_boot.sh'
    fake.write_text(FAKE_BOOT)
    fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
    src = tmp_path / 'install'
    src.mkdir()
    (src / 'seed.txt').write_text('install\n')
    env = clean_env(FAKE_DIR=str(tmp_path), FAKE_OUTCOMES=' '.join(outcomes),
                    SFCOP_WORK=str(tmp_path / 'work'))
    return fake, src, env


def rows(path):
    lines = path.read_text().splitlines()
    assert lines[0] == 'run\tclock\toutcome\tat_s\texit\tbackground\tlast_line'
    out = []
    for line in lines[1:]:
        run, clock, outcome, at, rc, bg, last = line.split('\t')
        assert re.fullmatch(r'\d\d:\d\d:\d\d', clock)
        out.append((run, outcome, at, rc, bg, last))
    return out


def test_each_outcome_is_classified(tmp_path):
    fake, src, env = setup(tmp_path, ['prompt', 'fault', 'throw', 'alive', 'other'])
    tsv = tmp_path / 'probe.tsv'
    r = subprocess.run(['sh', PROBE, '-s', str(src), '-w', '12', '-o', str(tsv), '-l', str(tmp_path / 'logs'),
                        '5', '--', str(fake), '@GAME@', '--watchdog', '@WATCHDOG@'],
                       capture_output=True, text=True, env=env)
    assert r.returncode == 0, r.stderr
    assert rows(tsv) == [
        ('1', 'prompt', '5.295', '0', 'space21.mod',
         '[boot] watchdog after 12 s: the boot reached the CD-key prompt'),
        ('2', 'fault', '0.311', '1', '-', '=== fault: signal 10, address 0x100000000004 at 0.311 s ==='),
        ('3', 'throw', '-', '1', '-', '[throw] guest C++ exceptions are not handled yet (Wave 1 B6): stopping'),
        ('4', 'alive', '-', '3', '-', '=== watchdog at 12.001 s ==='),
        ('5', 'other', '-', '2', '-', 'cannot enter the game directory'),
    ]
    # The fresh copies are removed afterwards and the install is untouched.
    assert not (tmp_path / 'logs' / 'game').exists()
    assert sorted(os.listdir(src)) == ['seed.txt']


def test_default_output_goes_under_the_work_directory(tmp_path):
    fake, src, env = setup(tmp_path, ['prompt'])
    r = subprocess.run(['sh', PROBE, '-s', str(src), '1', '--', str(fake), '@GAME@'],
                       capture_output=True, text=True, env=env)
    assert r.returncode == 0, r.stderr
    written = r.stdout.strip().splitlines()[-1].removeprefix('boot_probe: ')
    assert written.startswith(str(tmp_path / 'work' / 'F1' / 'probe') + os.sep)
    assert written.endswith(os.sep + 'probe.tsv')
    assert [row[1] for row in rows(type(tmp_path)(written))] == ['prompt']


def test_bad_usage_exits_2(tmp_path):
    fake, src, env = setup(tmp_path, ['prompt'])
    for args in (['x', '--', str(fake)],                         # N is not a number
                 ['1', str(fake)],                               # no --
                 ['1', '--', str(fake), '@GAME@'],               # @GAME@ without -s
                 ['-t', 'noon', '1', '--', str(fake)],           # -t is not epoch seconds
                 ['-s', str(tmp_path / 'missing'), '1', '--', str(fake)]):
        r = subprocess.run(['sh', PROBE] + args, capture_output=True, text=True, env=env)
        assert r.returncode == 2, (args, r.stdout, r.stderr)


@pytest.mark.skipif(sys.platform != 'darwin', reason='-t loads tools/fakeclock.c with dyld')
def test_t_sets_each_runs_clock(tmp_path):
    src = tmp_path / 'reader.c'
    src.write_text(CLOCK_READER)
    reader = tmp_path / 'reader'
    subprocess.run(['cc', '-o', str(reader), str(src)], check=True)
    logs = tmp_path / 'logs'
    # 2001-06-08 08:20:00 UTC, then one hour later.
    r = subprocess.run(['sh', PROBE, '-l', str(logs), '-t', '991988400', '-T', '3600', '2', '--', str(reader)],
                       capture_output=True, text=True, env=clean_env())
    assert r.returncode == 0, r.stderr
    for i, want in ((1, 991988400), (2, 991992000)):
        seen = [int(x) for x in (logs / ('run_%d.out' % i)).read_text().split()]
        # The offset is fixed just before the run starts; allow for the seconds a start takes.
        assert len(seen) == 3 and all(want <= s <= want + 5 for s in seen[:2]), (i, seen)
        # clock_gettime is left at the kernel's time, so timed waits keep their real deadlines.
        assert abs(seen[2] - time.time()) < 60, (i, seen)
