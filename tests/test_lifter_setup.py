"""cmake/lifter/setup.sh: the build folder's own patched pcrecomp (--copy) and the patch series.

Each test runs a copy of setup.sh beside a patches/ folder of its own, against a small local git
repository standing in for the pcrecomp clone: its revision is written into the copied script in
place of the pinned one. No network, no pcrecomp. Expected file contents are written out by hand.
"""
import os
import re
import shutil
import subprocess

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIFTER = os.path.join(REPO, 'cmake', 'lifter')
SETUP = os.path.join(LIFTER, 'setup.sh')

GIT_ENV = dict(os.environ, GIT_AUTHOR_NAME='Test', GIT_AUTHOR_EMAIL='test@example.invalid',
               GIT_COMMITTER_NAME='Test', GIT_COMMITTER_EMAIL='test@example.invalid',
               GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM='1')

# Two patches: the second applies only on top of the first, so the order is checked too.
PATCH_A = """\
--- a/greeting.txt
+++ b/greeting.txt
@@ -1,2 +1,2 @@
 hello
-world
+patched world
"""
PATCH_B = """\
--- a/greeting.txt
+++ b/greeting.txt
@@ -1,2 +1,3 @@
 hello
 patched world
+second line
"""
PATCH_A2 = """\
--- a/greeting.txt
+++ b/greeting.txt
@@ -1,2 +1,2 @@
 hello
-world
+other world
"""


def git(*args, cwd=None):
    return subprocess.run(['git', *args], cwd=cwd, env=GIT_ENV, check=True,
                          capture_output=True, text=True).stdout.strip()


@pytest.fixture
def source(tmp_path):
    """A clone standing in for pcrecomp: one commit holding greeting.txt. Returns (dir, rev)."""
    src = tmp_path / 'source'
    src.mkdir()
    git('init', '-q', str(src))
    (src / 'greeting.txt').write_text('hello\nworld\n')
    git('add', 'greeting.txt', cwd=src)
    git('commit', '-q', '-m', 'base', cwd=src)
    return src, git('rev-parse', 'HEAD', cwd=src)


def lifter(tmp_path, name, rev, patches, series):
    """A lifter folder `name`: setup.sh pinned to `rev`, the given patches and series text."""
    d = tmp_path / name
    (d / 'patches').mkdir(parents=True)
    text = open(SETUP).read()
    text, n = re.subn(r'^PCRECOMP_REV=[0-9a-f]{40}$', 'PCRECOMP_REV=' + rev, text, flags=re.M)
    assert n == 1
    (d / 'setup.sh').write_text(text)
    for fname, body in patches.items():
        (d / 'patches' / fname).write_text(body)
    (d / 'patches' / 'series').write_text(series)
    return d


def run_copy(lifter_dir, src, dest, stamp):
    return subprocess.run(['sh', str(lifter_dir / 'setup.sh'), '--copy', str(src), str(dest),
                           str(stamp)], env=GIT_ENV, capture_output=True, text=True)


def test_copy_applies_the_series_in_order_and_leaves_the_source_alone(tmp_path, source):
    src, rev = source
    d = lifter(tmp_path, 'lifter', rev, {'a.patch': PATCH_A, 'b.patch': PATCH_B},
               '# comment\na.patch\n\nb.patch\n')
    r = run_copy(d, src, tmp_path / 'build' / 'pcrecomp', tmp_path / 'build' / 'pcrecomp.stamp')
    assert r.returncode == 0, r.stderr
    assert (tmp_path / 'build' / 'pcrecomp' / 'greeting.txt').read_text() == \
        'hello\npatched world\nsecond line\n'
    assert git('rev-parse', 'HEAD', cwd=tmp_path / 'build' / 'pcrecomp') == rev
    assert re.fullmatch(r'[0-9a-f]{64}\n', (tmp_path / 'build' / 'pcrecomp.stamp').read_text())
    # The source: still the bare commit, nothing modified or added.
    assert (src / 'greeting.txt').read_text() == 'hello\nworld\n'
    assert git('status', '--porcelain', cwd=src) == ''


def test_an_unchanged_series_does_nothing(tmp_path, source):
    src, rev = source
    d = lifter(tmp_path, 'lifter', rev, {'a.patch': PATCH_A}, 'a.patch\n')
    dest, stamp = tmp_path / 'pcrecomp', tmp_path / 'pcrecomp.stamp'
    assert run_copy(d, src, dest, stamp).returncode == 0
    (dest / 'marker').write_text('kept')
    before = (stamp.stat().st_mtime_ns, (dest / 'greeting.txt').stat().st_mtime_ns)
    r = run_copy(d, src, dest, stamp)
    assert r.returncode == 0, r.stderr
    assert r.stdout == ''
    assert (dest / 'marker').read_text() == 'kept'
    assert (stamp.stat().st_mtime_ns, (dest / 'greeting.txt').stat().st_mtime_ns) == before


def test_a_changed_patch_makes_the_copy_again(tmp_path, source):
    src, rev = source
    d = lifter(tmp_path, 'lifter', rev, {'a.patch': PATCH_A}, 'a.patch\n')
    dest, stamp = tmp_path / 'pcrecomp', tmp_path / 'pcrecomp.stamp'
    assert run_copy(d, src, dest, stamp).returncode == 0
    first = stamp.read_text()
    (dest / 'marker').write_text('dropped')
    (d / 'patches' / 'a.patch').write_text(PATCH_A2)
    r = run_copy(d, src, dest, stamp)
    assert r.returncode == 0, r.stderr
    assert (dest / 'greeting.txt').read_text() == 'hello\nother world\n'
    assert not (dest / 'marker').exists()
    assert stamp.read_text() != first


def test_a_changed_order_makes_the_copy_again(tmp_path, source):
    src, rev = source
    d = lifter(tmp_path, 'lifter', rev, {'a.patch': PATCH_A, 'b.patch': PATCH_B},
               'a.patch\nb.patch\n')
    dest, stamp = tmp_path / 'pcrecomp', tmp_path / 'pcrecomp.stamp'
    assert run_copy(d, src, dest, stamp).returncode == 0
    (d / 'patches' / 'series').write_text('b.patch\na.patch\n')
    r = run_copy(d, src, dest, stamp)
    # b.patch does not apply to the bare commit: the copy is made again, in the new order, and
    # stops there, with no stamp left to say it is up to date.
    assert r.returncode != 0
    assert 'b.patch does not apply' in r.stderr
    assert not stamp.exists()


def test_two_build_folders_with_different_series_side_by_side(tmp_path, source):
    src, rev = source
    one = lifter(tmp_path, 'one', rev, {'a.patch': PATCH_A, 'b.patch': PATCH_B},
                 'a.patch\nb.patch\n')
    two = lifter(tmp_path, 'two', rev, {'a.patch': PATCH_A2}, 'a.patch\n')
    assert run_copy(one, src, tmp_path / 'b1' / 'pcrecomp', tmp_path / 'b1' / 'stamp').returncode == 0
    assert run_copy(two, src, tmp_path / 'b2' / 'pcrecomp', tmp_path / 'b2' / 'stamp').returncode == 0
    assert (tmp_path / 'b1' / 'pcrecomp' / 'greeting.txt').read_text() == \
        'hello\npatched world\nsecond line\n'
    assert (tmp_path / 'b2' / 'pcrecomp' / 'greeting.txt').read_text() == 'hello\nother world\n'
    assert (src / 'greeting.txt').read_text() == 'hello\nworld\n'


@pytest.mark.parametrize('patches, series, message', [
    ({'a.patch': PATCH_A, 'b.patch': PATCH_B}, 'a.patch\n', 'b.patch is not listed in patches/series'),
    ({'a.patch': PATCH_A}, 'a.patch\nc.patch\n', 'lists c.patch, which is not in'),
    ({'a.patch': PATCH_A}, 'a.patch\na.patch\n', 'lists a.patch more than once'),
])
def test_a_series_that_does_not_match_the_folder_stops(tmp_path, source, patches, series, message):
    src, rev = source
    d = lifter(tmp_path, 'lifter', rev, patches, series)
    r = run_copy(d, src, tmp_path / 'pcrecomp', tmp_path / 'stamp')
    assert r.returncode == 2   # configure treats status 2 as an error
    assert message in r.stderr
    assert not (tmp_path / 'pcrecomp').exists()


def test_the_copy_outlives_a_moved_or_collected_source(tmp_path, source):
    src, rev = source
    d = lifter(tmp_path, 'lifter', rev, {'a.patch': PATCH_A}, 'a.patch\n')
    dest, stamp = tmp_path / 'pcrecomp', tmp_path / 'pcrecomp.stamp'
    assert run_copy(d, src, dest, stamp).returncode == 0
    assert not (dest / '.git' / 'objects' / 'info' / 'alternates').exists()
    git('gc', '-q', '--prune=now', cwd=src)
    src.rename(tmp_path / 'moved')
    assert git('cat-file', '-t', rev, cwd=dest) == 'commit'
    assert (dest / 'greeting.txt').read_text() == 'hello\npatched world\n'
    # Up to date: nothing to do, the source is not needed.
    r = run_copy(d, src, dest, stamp)
    assert r.returncode == 0, r.stderr
    # Once the copy must be made again, the missing source is named with the fix.
    (d / 'patches' / 'a.patch').write_text(PATCH_A2)
    r = run_copy(d, src, dest, stamp)
    assert r.returncode == 1
    assert 'no pcrecomp clone at %s' % src in r.stderr
    assert 'configure again' in r.stderr


def test_a_damaged_copy_is_made_again(tmp_path, source):
    src, rev = source
    d = lifter(tmp_path, 'lifter', rev, {'a.patch': PATCH_A}, 'a.patch\n')
    dest, stamp = tmp_path / 'pcrecomp', tmp_path / 'pcrecomp.stamp'
    assert run_copy(d, src, dest, stamp).returncode == 0
    shutil.rmtree(dest / '.git')
    r = run_copy(d, src, dest, stamp)
    assert r.returncode == 0, r.stderr
    assert git('rev-parse', 'HEAD', cwd=dest) == rev
    assert (dest / 'greeting.txt').read_text() == 'hello\npatched world\n'


def test_a_source_without_the_revision_stops(tmp_path, source):
    src, _ = source
    d = lifter(tmp_path, 'lifter', 'f' * 40, {'a.patch': PATCH_A}, 'a.patch\n')
    r = run_copy(d, src, tmp_path / 'pcrecomp', tmp_path / 'stamp')
    assert r.returncode != 0
    assert 'does not hold ' + 'f' * 40 in r.stderr


def test_the_repository_series_lists_every_patch_once():
    listed = [ln.split('#')[0].strip() for ln in open(os.path.join(LIFTER, 'patches', 'series'))]
    listed = [p for p in listed if p]
    on_disk = sorted(f for f in os.listdir(os.path.join(LIFTER, 'patches')) if f.endswith('.patch'))
    assert sorted(listed) == on_disk
    assert len(set(listed)) == len(listed)
    # The manual lookup hook applies on top of the TIB hook.
    assert listed.index('pcrecomp-tib-hook.patch') < listed.index('pcrecomp-manual-lookup-hook.patch')


def test_the_lift_key_covers_the_series():
    text = open(os.path.join(LIFTER, 'lift.sh')).read()
    assert '"$HERE"/patches/series' in text


# --- pcrecomp_copy.py: the copies tools/scr2native and the objlift tests make outside a build ----

def keyed(tmp_path, src, rev):
    """pcrecomp_copy as it runs against the stand-in clone, which needs the file it looks for."""
    import importlib.util
    spec = importlib.util.spec_from_file_location('pcrecomp_copy', os.path.join(LIFTER, 'pcrecomp_copy.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    (src / 'tools' / 'lift').mkdir(parents=True)
    (src / 'tools' / 'lift' / 'lift32.py').write_text('')
    git('add', 'tools', cwd=src)
    git('commit', '-q', '-m', 'lifter', cwd=src)
    return mod, git('rev-parse', 'HEAD', cwd=src)


def test_two_patch_sets_keep_two_keyed_copies_side_by_side_in_one_root(tmp_path, source):
    src, _ = source
    mod, rev = keyed(tmp_path, src, None)
    one = lifter(tmp_path, 'one', rev, {'a.patch': PATCH_A, 'b.patch': PATCH_B}, 'a.patch\nb.patch\n')
    two = lifter(tmp_path, 'two', rev, {'a.patch': PATCH_A2}, 'a.patch\n')
    root = tmp_path / 'shared'
    d1, k1 = mod.patched_copy(str(src), str(root), str(one))
    d2, k2 = mod.patched_copy(str(src), str(root), str(two))
    assert k1 != k2
    assert sorted(n for n in os.listdir(root) if not n.startswith('.lock-')) == \
        sorted(['pcrecomp-' + k1[:16], 'pcrecomp-' + k2[:16]])
    assert os.path.isfile(os.path.join(d1, '.pcrecomp-copy.stamp'))
    assert os.path.isfile(os.path.join(d2, '.pcrecomp-copy.stamp'))
    # Asking for the first again, after the second was made, finds it as it was.
    (tmp_path / 'shared' / ('pcrecomp-' + k1[:16]) / 'marker').write_text('kept')
    assert mod.patched_copy(str(src), str(root), str(one)) == (d1, k1)
    assert open(os.path.join(d1, 'marker')).read() == 'kept'
    assert open(os.path.join(d1, 'greeting.txt')).read() == 'hello\npatched world\nsecond line\n'
    assert open(os.path.join(d2, 'greeting.txt')).read() == 'hello\nother world\n'
    assert (src / 'greeting.txt').read_text() == 'hello\nworld\n'


def test_the_key_follows_a_patch_and_the_order(tmp_path, source):
    src, rev = source
    import importlib.util
    spec = importlib.util.spec_from_file_location('pcrecomp_copy', os.path.join(LIFTER, 'pcrecomp_copy.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    d = lifter(tmp_path, 'lifter', rev, {'a.patch': PATCH_A, 'b.patch': PATCH_B}, 'a.patch\nb.patch\n')
    first = mod.patch_set_key(str(d))
    (d / 'patches' / 'series').write_text('b.patch\na.patch\n')
    second = mod.patch_set_key(str(d))
    (d / 'patches' / 'a.patch').write_text(PATCH_A2)
    third = mod.patch_set_key(str(d))
    assert len({first, second, third}) == 3


def test_a_missing_clone_or_a_patch_that_does_not_apply_leaves_no_copy(tmp_path, source):
    src, _ = source
    mod, rev = keyed(tmp_path, src, None)
    root = tmp_path / 'shared'
    with pytest.raises(mod.CopyError, match='no pcrecomp clone at .*cmake/lifter/setup.sh'):
        mod.patched_copy(str(tmp_path / 'nowhere'), str(root))
    d = lifter(tmp_path, 'lifter', rev, {'b.patch': PATCH_B}, 'b.patch\n')
    with pytest.raises(mod.CopyError, match='b.patch does not apply'):
        mod.patched_copy(str(src), str(root), str(d))
    assert os.listdir(root) == []


def test_a_folder_left_without_its_stamp_is_replaced(tmp_path, source):
    """A crash after the copy was moved into place but before it was complete, or a copy from an
    older layout (its stamp beside it), leaves a folder under the copy's name with no stamp inside.
    The next run replaces it instead of failing on it, and later runs find the new copy."""
    src, _ = source
    mod, rev = keyed(tmp_path, src, None)
    d = lifter(tmp_path, 'lifter', rev, {'a.patch': PATCH_A}, 'a.patch\n')
    root = tmp_path / 'shared'
    key = mod.patch_set_key(str(d))
    leftover = root / ('pcrecomp-' + key[:16])
    (leftover / 'half').mkdir(parents=True)
    (leftover / 'greeting.txt').write_text('hello\n')
    (root / ('pcrecomp-' + key[:16] + '.stamp')).write_text('old layout\n')
    path, k = mod.patched_copy(str(src), str(root), str(d))
    assert (path, k) == (str(leftover), key)
    assert (leftover / 'greeting.txt').read_text() == 'hello\npatched world\n'
    assert not (leftover / 'half').exists()
    assert (leftover / '.pcrecomp-copy.stamp').is_file()
    assert sorted(n for n in os.listdir(root) if not n.startswith('.lock-')) == ['pcrecomp-' + key[:16]]
    # Found as it is from now on.
    (leftover / 'marker').write_text('kept')
    assert mod.patched_copy(str(src), str(root), str(d)) == (path, key)
    assert (leftover / 'marker').read_text() == 'kept'


def test_a_copy_another_run_published_first_is_kept(tmp_path, source, monkeypatch):
    """Two runs make the same copy at once: the one that publishes second keeps the first one's
    copy and discards its own."""
    src, _ = source
    mod, rev = keyed(tmp_path, src, None)
    d = lifter(tmp_path, 'lifter', rev, {'a.patch': PATCH_A}, 'a.patch\n')
    root = tmp_path / 'shared'
    first, key = mod.patched_copy(str(src), str(root), str(d))
    (tmp_path / 'shared' / ('pcrecomp-' + key[:16]) / 'marker').write_text('first')
    # The second run had already looked (no copy then) and made its own; it publishes now.
    real_isfile = os.path.isfile
    looked = []

    def isfile_once_missing(p):
        if p.endswith('.pcrecomp-copy.stamp') and p.startswith(first) and not looked:
            looked.append(p)
            return False
        return real_isfile(p)
    monkeypatch.setattr(mod.os.path, 'isfile', isfile_once_missing)
    assert mod.patched_copy(str(src), str(root), str(d)) == (first, key)
    assert open(os.path.join(first, 'marker')).read() == 'first'
    assert sorted(n for n in os.listdir(root) if not n.startswith('.lock-')) == ['pcrecomp-' + key[:16]]


def test_series_names_are_matched_as_text_not_as_patterns(tmp_path, source):
    """The '.' of a.patch is not a wildcard: a series that also lists aXpatch names each once."""
    src, rev = source
    d = lifter(tmp_path, 'lifter', rev, {'a.patch': PATCH_A, 'aXpatch': PATCH_B}, 'a.patch\naXpatch\n')
    r = run_copy(d, src, tmp_path / 'pcrecomp', tmp_path / 'stamp')
    assert r.returncode == 0, r.stderr
    assert (tmp_path / 'pcrecomp' / 'greeting.txt').read_text() == 'hello\npatched world\nsecond line\n'
