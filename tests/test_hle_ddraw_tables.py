"""The DirectDraw and Direct3D guest shim tables match src/hle/surface.tsv, and the checker and the
frame diff tool of the proof run work (src/hle/ddraw4/tools/). Reads committed files only."""
import pathlib
import subprocess
import sys

import pytest

ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOLS = ROOT / 'src' / 'hle' / 'ddraw4' / 'tools'
sys.path.insert(0, str(TOOLS))
import check_tables  # noqa: E402


def test_tables_match_surface():
    compared, rows, problems = check_tables.check()
    assert problems == []
    assert compared == rows == 260  # 257 vtable slots of 11 interfaces and 3 DDRAW.DLL exports


def test_checker_runs_as_a_script():
    done = subprocess.run([sys.executable, str(TOOLS / 'check_tables.py')], capture_output=True, text=True)
    assert done.returncode == 0, done.stdout
    assert done.stdout.startswith('ok: 260 shim table entries')


@pytest.mark.parametrize('old,new,expect', [
    ('shim<ddCompact, 4>', 'shim<ddCompact, 8>', 'purges 8 bytes'),
    ('{"sfcop!" iface "::Compact", shim<ddCompact, 4>},', '', 'shims'),
])
def test_checker_sees_a_wrong_table(tmp_path, monkeypatch, old, new, expect):
    source = (ROOT / 'src' / 'hle' / 'ddraw4' / 'Ddraw4Guest.cpp').read_text()
    assert old in source
    broken = tmp_path / 'Ddraw4Guest.cpp'
    broken.write_text(source.replace(old, new, 1))
    monkeypatch.setattr(check_tables, 'GUEST_FILES',
                        [broken, ROOT / 'src' / 'hle' / 'd3d3' / 'D3d3Guest.cpp'])
    _, _, problems = check_tables.check()
    assert problems
    assert any(expect in p for p in problems), problems


def test_framediff_self_test():
    done = subprocess.run([sys.executable, str(TOOLS / 'framediff.py'), '--self-test'],
                          capture_output=True, text=True)
    assert done.returncode == 0, done.stderr
    assert 'self-test passed' in done.stdout
