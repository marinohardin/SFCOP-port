"""A patched pcrecomp for tools that run outside a build folder (tools/scr2native, the objlift
tests): cmake/lifter/setup.sh --copy, named by the patch set it carries.

    patched_copy(source, root) -> (path, key)

`key` is a hash of the pinned revision, setup.sh, and each patch in patches/series with its name
and content, in order. The copy lives in root/pcrecomp-<key 16 hex>: checkouts or test runs whose
series differ get different folders, so neither ever removes the other's copy. A copy is made in a
private folder and renamed into place once complete, so two runs that need the same one at once
never see it half made; the second's work is discarded. A copy counts only with its stamp inside
it (READY), written before that rename, so a folder left without one by a crash is replaced at the
next run. The repair and the rename happen under a lock file in the root, so a run never removes
a copy another run has just published. `source` is only read.

Raises CopyError naming the fix when the clone is missing or a patch does not apply.
Standard library only.
"""
import hashlib
import os
import re
import shutil
import subprocess
from contextlib import contextmanager

try:
    import fcntl
except ImportError:                 # Windows: no other build shares a work folder there
    fcntl = None

LIFTER = os.path.dirname(os.path.abspath(__file__))


class CopyError(Exception):
    pass


def series(lifter=LIFTER):
    """The patch names patches/series lists, in order (comments and blank lines dropped)."""
    with open(os.path.join(lifter, 'patches', 'series'), encoding='utf-8') as f:
        return [n for n in (ln.split('#')[0].strip() for ln in f) if n]


def patch_set_key(lifter=LIFTER):
    """One hash of the revision, setup.sh, and each listed patch's name and content, in order."""
    setup = os.path.join(lifter, 'setup.sh')
    text = open(setup, encoding='utf-8').read()
    rev = re.search(r'^PCRECOMP_REV=([0-9a-f]{40})$', text, re.M).group(1)
    h = hashlib.sha256()
    h.update(('pcrecomp %s\n' % rev).encode())
    h.update(hashlib.sha256(text.encode()).hexdigest().encode() + b'\n')
    for name in series(lifter):
        with open(os.path.join(lifter, 'patches', name), 'rb') as f:
            h.update(('%s %s\n' % (name, hashlib.sha256(f.read()).hexdigest())).encode())
    return h.hexdigest()


@contextmanager
def _publish_lock(path):
    """Holds an exclusive lock on `path` (created if missing) while copies are published."""
    with open(path, 'a') as f:
        if fcntl is not None:
            fcntl.flock(f, fcntl.LOCK_EX)
        try:
            yield
        finally:
            if fcntl is not None:
                fcntl.flock(f, fcntl.LOCK_UN)


READY = '.pcrecomp-copy.stamp'      # inside a copy: written before the copy is moved into place


def patched_copy(source, root, lifter=LIFTER):
    if not os.path.isfile(os.path.join(source, 'tools', 'lift', 'lift32.py')):
        raise CopyError('no pcrecomp clone at %s: run cmake/lifter/setup.sh %s, or give the clone '
                        '(--pcrecomp, SFCOP_PCRECOMP_DIR)' % (source, source))
    key = patch_set_key(lifter)
    dest = os.path.join(root, 'pcrecomp-' + key[:16])
    if os.path.isfile(os.path.join(dest, READY)):
        return dest, key
    os.makedirs(root, exist_ok=True)
    private = os.path.join(root, '.making-%s-%d' % (key[:16], os.getpid()))
    shutil.rmtree(private, ignore_errors=True)
    made = os.path.join(private, 'pcrecomp')
    proc = subprocess.run(['sh', os.path.join(lifter, 'setup.sh'), '--copy', source, made,
                           os.path.join(private, 'stamp')], capture_output=True, text=True)
    if proc.returncode != 0:
        shutil.rmtree(private, ignore_errors=True)
        raise CopyError((proc.stderr.strip().splitlines() or ['cmake/lifter/setup.sh --copy failed'])[-1])
    # The stamp goes inside the copy, so one rename publishes a complete copy and its stamp together.
    os.rename(os.path.join(private, 'stamp'), os.path.join(made, READY))
    with _publish_lock(os.path.join(root, '.lock-' + key[:16])):
        if not os.path.isfile(os.path.join(dest, READY)):
            if os.path.isdir(dest):
                # A folder without the stamp is not a copy: left by a run that stopped part way (a
                # crash, an older layout). Under the lock no other run is publishing, so remove it.
                aside = os.path.join(root, '.stale-%s-%d' % (key[:16], os.getpid()))
                os.rename(dest, aside)
                shutil.rmtree(aside, ignore_errors=True)
            os.rename(made, dest)
    shutil.rmtree(private, ignore_errors=True)
    if os.path.isfile(dest + '.stamp'):                     # the stamp beside it, an older layout
        os.remove(dest + '.stamp')
    return dest, key
