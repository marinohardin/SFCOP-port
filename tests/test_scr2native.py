"""scr2native (tools/scr2native): the PE reader, the import check, the guest base, the guest image it
writes, --check, and the pipeline from a script to an installed module.

The tests build their own DLL (make_dll below): a few bytes of code, two exports, two imports and
three fixups, laid out as MSVC lays out a mission. No retail byte is read except by the tests marked
`retail`, which read the oracle mission (oracle/scr/build.sh) and are skipped where it is absent. The
lift itself needs Docker and minutes per script, so the pipeline tests stand a fake lift in for it.
"""
import json
import os
import pathlib
import platform
import shutil
import struct
import subprocess
import sys

import pytest

ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOL = ROOT / 'tools' / 'scr2native'
FIXTURES = ROOT / 'oracle' / 'trace' / 'fixtures'
sys.path.insert(0, str(TOOL))
import pe32  # noqa: E402
import scr2native  # noqa: E402
from harness.lib import paths  # noqa: E402

ORACLE = pathlib.Path(paths.WORK) / 'oracle' / 'build' / 'out' / 'OracleSkirmish.scr'
retail = pytest.mark.retail
need_oracle = pytest.mark.skipif(not ORACLE.is_file(), reason='the oracle mission is not built here')

BASE = 0x10000000
TEXT, RDATA, DATA, RELOC = 0x1000, 0x2000, 0x3000, 0x4000


def make_dll(imports=(('KERNEL32.dll', 'HeapAlloc'), ('USER32.dll', 'wsprintfA')), machine=0x014C,
             characteristics=0x2102):
    """A PE32 DLL at 0x10000000 with sections .text, .rdata, .data and .reloc:
      .text   0x1000 DllMain (ret 0Ch), 0x1004 MissionSpecEntry, 0x1008 ScriptEntry,
              0x100C mov eax, [0x10003000]          (a fixup in code, to data)
      .rdata  0x2000 a vtable holding 0x10001008     (a fixup in data, to code)
              0x2004 a pointer to 0x10003004         (a fixup in data, to data)
              then the import directory, its names and the export directory
      .data   0x3000 two words
    """
    text = bytearray(0x200)
    text[0x0:0x3] = b'\xC2\x0C\x00'
    text[0x4] = 0xC3
    text[0x8] = 0xC3
    text[0xC:0x11] = b'\xA1' + struct.pack('<I', BASE + DATA)
    text[0x11] = 0xC3
    rdata = bytearray(0x400)
    struct.pack_into('<II', rdata, 0, BASE + TEXT + 0x8, BASE + DATA + 4)

    # Imports: descriptors at 0x2010, then for each DLL its lookup table, its IAT and the names.
    dlls = []
    for dll, name in imports:
        if not dlls or dlls[-1][0] != dll:
            dlls.append((dll, []))
        dlls[-1][1].append(name)
    p = 0x10 + 20 * (len(dlls) + 1)
    descriptors = []
    for dll, names in dlls:
        ilt = p
        iat = ilt + 4 * (len(names) + 1)
        p = iat + 4 * (len(names) + 1)
        for i, name in enumerate(names):
            struct.pack_into('<I', rdata, ilt + 4 * i, RDATA + p)
            struct.pack_into('<I', rdata, iat + 4 * i, RDATA + p)
            entry = b'\0\0' + name.encode() + b'\0'
            rdata[p:p + len(entry)] = entry
            p += (len(entry) + 1) & ~1
        dll_name = dll.encode() + b'\0'
        rdata[p:p + len(dll_name)] = dll_name
        descriptors.append((RDATA + ilt, RDATA + p, RDATA + iat))
        p += (len(dll_name) + 3) & ~3
    for i, (ilt, name, iat) in enumerate(descriptors):
        struct.pack_into('<IIIII', rdata, 0x10 + 20 * i, ilt, 0, 0, name, iat)
    imports_rva = RDATA + 0x10

    # Exports, sorted by name as a linker writes them.
    exports_rva = RDATA + p
    funcs = p + 40
    names = funcs + 8
    ords = names + 8
    strings = ords + 4
    mse = strings
    se = mse + len(b'MissionSpecEntry\0')
    dllname = se + len(b'ScriptEntry\0')
    struct.pack_into('<IIHHIIIIIII', rdata, p, 0, 0, 0, 0, RDATA + dllname, 1, 2, 2,
                     RDATA + funcs, RDATA + names, RDATA + ords)
    struct.pack_into('<II', rdata, funcs, TEXT + 4, TEXT + 8)
    struct.pack_into('<II', rdata, names, RDATA + mse, RDATA + se)
    struct.pack_into('<HH', rdata, ords, 0, 1)
    tail = b'MissionSpecEntry\0ScriptEntry\0Fixture.scr\0'
    rdata[strings:strings + len(tail)] = tail
    exports_size = strings + len(tail) - p

    data = bytearray(0x200)
    struct.pack_into('<II', data, 0, 0x11111111, 0x22222222)
    reloc = bytearray(0x200)
    blocks = b''
    for page, offsets in ((TEXT, [0xD]), (RDATA, [0x0, 0x4])):
        entries = b''.join(struct.pack('<H', 0x3000 | o) for o in offsets)
        if len(entries) % 4:
            entries += struct.pack('<H', 0)  # an ABSOLUTE pad, as linkers write
        blocks += struct.pack('<II', page, 8 + len(entries)) + entries
    reloc[:len(blocks)] = blocks

    sections = [(b'.text', TEXT, 0x20, text, 0x60000020), (b'.rdata', RDATA, 0x400, rdata, 0x40000040),
                (b'.data', DATA, 8, data, 0xC0000040), (b'.reloc', RELOC, len(blocks), reloc, 0x42000040)]
    headers = bytearray(0x400)
    headers[0:2] = b'MZ'
    struct.pack_into('<I', headers, 0x3C, 0x80)
    headers[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', headers, 0x84, machine, len(sections), 0, 0, 0, 0xE0, characteristics)
    opt = 0x98
    struct.pack_into('<HBBIIIIII', headers, opt, 0x010B, 6, 0, 0x200, 0x600, 0, TEXT, TEXT, RDATA)
    struct.pack_into('<III', headers, opt + 28, BASE, 0x1000, 0x200)
    struct.pack_into('<II', headers, opt + 56, 0x5000, 0x400)
    struct.pack_into('<I', headers, opt + 92, 16)
    for i, (rva, size) in enumerate([(exports_rva, exports_size), (imports_rva, 20 * (len(dlls) + 1)),
                                     (0, 0), (0, 0), (0, 0), (RELOC, len(blocks))]):
        struct.pack_into('<II', headers, opt + 96 + 8 * i, rva, size)
    table = opt + 0xE0
    raw = 0x400
    body = b''
    for i, (name, rva, vsize, content, chars) in enumerate(sections):
        struct.pack_into('<8sIIIIIIHHI', headers, table + 40 * i, name, vsize, rva, len(content), raw,
                         0, 0, 0, 0, chars)
        raw += len(content)
        body += bytes(content)
    return bytes(headers) + body


@pytest.fixture
def fixture_dll():
    return pe32.Dll(make_dll())


# --- the PE reader ----------------------------------------------------------------------------

def test_the_reader_finds_exports_imports_and_fixups(fixture_dll):
    assert fixture_dll.image_base == 0x10000000
    assert fixture_dll.entry_rva == 0x1000
    assert fixture_dll.exports() == {'MissionSpecEntry': 0x1004, 'ScriptEntry': 0x1008}
    assert [(i.full, i.iat_rva) for i in fixture_dll.imports()] == [
        ('KERNEL32.DLL!HeapAlloc', 0x2054), ('USER32.DLL!wsprintfA', 0x2080)]
    assert fixture_dll.relocations() == [0x100D, 0x2000, 0x2004]


def test_rebasing_moves_every_fixup_and_the_image_base(fixture_dll):
    moved = fixture_dll.rebased(0x60300000)
    assert moved.image_base == 0x60300000
    assert struct.unpack('<I', moved.read(0x100D, 4))[0] == 0x60303000
    assert struct.unpack('<I', moved.read(0x2000, 4))[0] == 0x60301008
    assert struct.unpack('<I', moved.read(0x2004, 4))[0] == 0x60303004
    assert moved.read(0x3000, 8) == fixture_dll.read(0x3000, 8)  # data that is not a fixup stays
    assert moved.exports() == fixture_dll.exports()


def test_code_pointers_held_in_data_seed_the_lift_and_code_fixups_do_not(fixture_dll):
    assert fixture_dll.code_pointer_seeds() == [0x10001008]
    assert fixture_dll.rebased(0x60000000).code_pointer_seeds() == [0x60001008]


def test_the_image_segments_are_the_headers_and_each_section_zero_filled(fixture_dll):
    segments = fixture_dll.image_segments()
    assert [(rva, size) for rva, size, _ in segments] == [
        (0, 0x400), (0x1000, 0x20), (0x2000, 0x400), (0x3000, 8), (0x4000, 0x18)]
    assert segments[1][2][:3] == b'\xC2\x0C\x00'


@pytest.mark.parametrize('data, message', [
    (b'XX' + bytes(200), 'no MZ header'),
    (make_dll(machine=0x8664), 'machine 0x8664, not i386'),
    (make_dll(characteristics=0x0102), 'not a DLL'),
    (make_dll()[:0x90], 'truncated'),
])
def test_a_file_that_is_not_a_32_bit_dll_is_refused_with_the_reason(data, message):
    with pytest.raises(pe32.PeError, match=message):
        pe32.Dll(data)


# --- reading, checking and placing a script ---------------------------------------------------

def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


def test_a_script_without_the_contract_exports_is_an_input_error(tmp_path):
    data = bytearray(make_dll())
    data = bytes(data).replace(b'ScriptEntry\0', b'ScriptEntrx\0')
    with pytest.raises(scr2native.Stop) as stop:
        scr2native.read_scr(str(write(tmp_path / 'X.scr', data)))
    assert stop.value.status == scr2native.EXIT_INPUT
    assert str(stop.value) == 'X.scr does not export ScriptEntry'


def test_the_first_import_off_the_allow_list_refuses_the_script_with_the_loaders_message():
    dll = pe32.Dll(make_dll(imports=(('KERNEL32.dll', 'HeapAlloc'), ('KERNEL32.dll', 'CreateProcessA'),
                                     ('WSOCK32.dll', 'connect'))))
    with pytest.raises(scr2native.Stop) as stop:
        scr2native.check_imports('Fixture', dll.imports(), {'KERNEL32.DLL!HeapAlloc'})
    assert stop.value.status == scr2native.EXIT_REFUSED
    assert str(stop.value) == ('refused: Fixture: imports KERNEL32.DLL!CreateProcessA, which is outside '
                               'the scripting library and the C runtime')


def test_each_script_takes_the_slot_of_its_place_among_its_folders_scripts(tmp_path):
    for name in ('b.scr', 'A.scr', 'C.SCR', 'notes.txt'):
        write(tmp_path / name, b'')
    assert scr2native.guest_base(str(tmp_path / 'A.scr'), None) == 0x60000000
    assert scr2native.guest_base(str(tmp_path / 'C.SCR'), None) == 0x60100000
    assert scr2native.guest_base(str(tmp_path / 'b.scr'), None) == 0x60200000
    assert scr2native.guest_base(str(tmp_path / 'b.scr'), 0x30000000) == 0x30000000


@pytest.mark.parametrize('base', [0x30001000, 0x10000000, 0x7F000000])
def test_a_base_off_the_granule_or_outside_the_modules_range_is_refused(tmp_path, base):
    with pytest.raises(scr2native.Stop) as stop:
        scr2native.guest_base(str(tmp_path / 'A.scr'), base)
    assert stop.value.status == scr2native.EXIT_INPUT


def test_an_image_larger_than_its_slot_needs_an_explicit_base(fixture_dll):
    fixture_dll.size_of_image = 0x100001
    with pytest.raises(scr2native.Stop, match='more than a slot'):
        scr2native.image_size(fixture_dll, 0x60000000, None)
    assert scr2native.image_size(fixture_dll, 0x60000000, 0x60000000) == 0x110000


# --- the guest image ------------------------------------------------------------------------

def test_the_guest_image_lists_the_segments_the_import_slots_and_the_entry_points(tmp_path, fixture_dll):
    moved = fixture_dll.rebased(0x60300000)
    dispatch = {0x60301000: 'sub_60301000', 0x60301004: 'sub_60301004', 0x60301008: 'sub_60301008'}
    out = tmp_path / 'module_image.c'
    scr2native.emit_image(str(out), moved, 0x60300000, 0x10000, moved.imports(), moved.exports(), dispatch)
    text = out.read_text()
    assert '{0x00002054u, "KERNEL32.DLL!HeapAlloc"}' in text
    assert '{0x00002080u, "USER32.DLL!wsprintfA"}' in text
    assert 'sfcop_module_guest' in text
    for value in ('0x60300000u', '0x00010000u', '0x60301000u', '0x60301004u', '0x60301008u'):
        assert value in text


# --- the pipeline, with a fake lift ------------------------------------------------------------

@pytest.fixture
def fake_tools(monkeypatch):
    """The lift writes a dispatch table for the entry points; the compiler writes a stand-in
    library; the symbol check passes."""
    def lift(work, name, rebased, exports, pcrecomp, log):
        gen = os.path.join(work, 'out', 'gen')
        os.makedirs(gen, exist_ok=True)
        base = rebased.image_base
        rows = ''.join('  { 0x%08Xu, sub_%08X },\n' % (base + rva, base + rva)
                       for rva in sorted(set(exports.values()) | {rebased.entry_rva}))
        open(os.path.join(gen, 'recomp_dispatch.c'), 'w').write(rows)
        open(os.path.join(work, 'lift.key'), 'w').write('fake\n')
        return gen

    def compile_module(work, name, gen, imports, pcrecomp, jobs, log):
        lib = os.path.join(work, 'build', name + scr2native.library_suffix())
        os.makedirs(os.path.dirname(lib), exist_ok=True)
        open(lib, 'wb').write(b'library')
        return lib

    monkeypatch.setattr(scr2native, 'lift', lift)
    monkeypatch.setattr(scr2native, 'compile_module', compile_module)
    monkeypatch.setattr(scr2native, 'verify', lambda *a: None)


def run_tool(tmp_path, *extra, data=None):
    script = write(tmp_path / 'scripts' / 'Fixture.scr', data or make_dll())
    return scr2native.main([str(script), '-o', str(tmp_path / 'modules'),
                            '--work', str(tmp_path / 'work')] + list(extra))


def test_a_translated_script_is_installed_with_its_manifest_and_its_lift_cleaned(tmp_path, fake_tools, capsys):
    assert run_tool(tmp_path) == scr2native.EXIT_OK
    module = tmp_path / 'modules' / 'Fixture'
    assert sorted(p.name for p in module.iterdir()) == sorted(['module.json', 'Fixture' + scr2native.library_suffix()])
    manifest = json.loads((module / 'module.json').read_text())
    assert manifest == {'name': 'Fixture', 'source_sha256': pe32.sha256(make_dll()),
                        'translator_version': scr2native.TRANSLATOR_VERSION}
    assert sorted(p.name for p in (tmp_path / 'work' / 'Fixture').iterdir()) == ['result.json', 'scr2native.log']
    assert 'Fixture: 0x60000000, 3 functions, 2 imports' in capsys.readouterr().out


def test_keep_work_keeps_the_lift_and_the_objects(tmp_path, fake_tools):
    assert run_tool(tmp_path, '--keep-work') == scr2native.EXIT_OK
    assert (tmp_path / 'work' / 'Fixture' / 'module_image.c').is_file()
    assert (tmp_path / 'work' / 'Fixture' / 'lift.key').is_file()


def test_a_foreign_import_stops_before_the_lift_and_writes_nothing(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(scr2native, 'lift', lambda *a: pytest.fail('lifted a refused script'))
    data = make_dll(imports=(('KERNEL32.dll', 'CreateProcessA'),))
    assert run_tool(tmp_path, data=data) == scr2native.EXIT_REFUSED
    assert 'refused: Fixture: imports KERNEL32.DLL!CreateProcessA' in capsys.readouterr().err
    assert not (tmp_path / 'modules').exists()


def test_an_export_the_lift_missed_refuses_the_module(tmp_path, fake_tools, monkeypatch):
    real = scr2native.dispatch_entries
    monkeypatch.setattr(scr2native, 'dispatch_entries',
                        lambda gen: {a: n for a, n in real(gen).items() if a != 0x60001008})
    assert run_tool(tmp_path) == scr2native.EXIT_REFUSED
    assert not (tmp_path / 'modules' / 'Fixture').exists()


def test_a_missing_pcrecomp_clone_is_a_missing_tool(tmp_path):
    assert run_tool(tmp_path, '--pcrecomp', str(tmp_path / 'nowhere')) == scr2native.EXIT_TOOL


def test_check_and_trace_go_together(tmp_path):
    with pytest.raises(SystemExit) as stop:
        run_tool(tmp_path, '--check', str(FIXTURES / 'expected.trace'))
    assert stop.value.code == 2


def test_check_installs_the_module_when_the_traces_agree(tmp_path, fake_tools):
    assert run_tool(tmp_path, '--check', str(FIXTURES / 'expected.trace'),
                    '--trace', str(FIXTURES / 'actual_same.trace')) == scr2native.EXIT_OK
    assert (tmp_path / 'modules' / 'Fixture' / 'module.json').is_file()


def test_check_writes_no_module_when_a_tick_differs(tmp_path, fake_tools, capsys):
    assert run_tool(tmp_path, '--check', str(FIXTURES / 'expected.trace'),
                    '--trace', str(FIXTURES / 'actual_tick3.trace')) == scr2native.EXIT_MISMATCH
    err = capsys.readouterr().err
    assert 'the trace differs from the reference; no module written' in err
    assert not (tmp_path / 'modules' / 'Fixture').exists()
    assert not list((tmp_path / 'modules').glob('.*'))  # no staging left behind


def test_check_against_a_trace_of_another_input_is_an_input_error(tmp_path, fake_tools):
    assert run_tool(tmp_path, '--check', str(FIXTURES / 'expected.trace'),
                    '--trace', str(FIXTURES / 'actual_header_input.trace')) == scr2native.EXIT_INPUT
    assert not (tmp_path / 'modules' / 'Fixture').exists()


def test_a_second_translation_replaces_the_installed_module(tmp_path, fake_tools):
    assert run_tool(tmp_path) == scr2native.EXIT_OK
    (tmp_path / 'modules' / 'Fixture' / 'stale').write_text('x')
    assert run_tool(tmp_path) == scr2native.EXIT_OK
    assert not (tmp_path / 'modules' / 'Fixture' / 'stale').exists()


# --- --all ---------------------------------------------------------------------------------------

def make_install(tmp_path, scripts):
    """A stand-in install: Assets/Scripts/<name>.scr for each (name, bytes)."""
    folder = tmp_path / 'install' / 'Assets' / 'Scripts'
    folder.mkdir(parents=True, exist_ok=True)
    for name, data in scripts.items():
        write(folder / (name + '.scr'), data)
    return tmp_path / 'install'


def run_all(tmp_path, install):
    return scr2native.main(['--all', str(install), '-o', str(tmp_path / 'modules'),
                            '--work', str(tmp_path / 'work')])


def results(tmp_path):
    rows = {}
    for line in (tmp_path / 'work' / 'results.md').read_text().splitlines():
        cells = [c.strip() for c in line.strip('|').split('|')]
        if len(cells) == 8 and cells[0] not in ('Script', '---'):
            rows[cells[0]] = cells
    return rows


def test_all_translates_each_script_records_a_refusal_and_goes_on(tmp_path, fake_tools, capsys):
    install = make_install(tmp_path, {
        'A_Good': make_dll(),
        'B_Foreign': make_dll(imports=(('KERNEL32.dll', 'CreateProcessA'),)),
        'C_Good': make_dll()})
    assert run_all(tmp_path, install) == scr2native.EXIT_REFUSED
    rows = results(tmp_path)
    assert list(rows) == ['A_Good', 'B_Foreign', 'C_Good']
    assert rows['A_Good'][1] == '0x60000000' and rows['C_Good'][1] == '0x60200000'
    assert rows['A_Good'][4] == '3' and rows['A_Good'][7] == 'translated'  # functions, outcome
    assert rows['B_Foreign'][7].startswith('refused: refused: B_Foreign: imports KERNEL32.DLL!CreateProcessA')
    assert rows['B_Foreign'][4] == '-'
    assert (tmp_path / 'modules' / 'C_Good' / 'module.json').is_file()
    assert not (tmp_path / 'modules' / 'B_Foreign').exists()
    out = capsys.readouterr().out
    assert 'scr2native --all: 3 scripts, 2 modules (0 kept, 2 translated), 1 not translated;' in out
    assert 'modules ' in out and ' MB, work files ' in out and 'this run ' in out


def test_all_keeps_a_current_module_without_lifting_it_again(tmp_path, fake_tools, monkeypatch):
    install = make_install(tmp_path, {'A_Good': make_dll(), 'B_Good': make_dll()})
    assert run_all(tmp_path, install) == scr2native.EXIT_OK
    monkeypatch.setattr(scr2native, 'lift', lambda *a: pytest.fail('lifted a current module'))
    assert run_all(tmp_path, install) == scr2native.EXIT_OK
    rows = results(tmp_path)
    assert rows['A_Good'][7].startswith('kept') and rows['B_Good'][7].startswith('kept')
    assert rows['A_Good'][4] == '3'  # the functions, from the first translation's record


def test_all_translates_again_a_module_made_from_another_script_or_translator(tmp_path, fake_tools):
    install = make_install(tmp_path, {'A_Good': make_dll(), 'B_Good': make_dll()})
    assert run_all(tmp_path, install) == scr2native.EXIT_OK
    manifest = tmp_path / 'modules' / 'A_Good' / 'module.json'
    old = json.loads(manifest.read_text())
    manifest.write_text(json.dumps({**old, 'source_sha256': '0' * 64}))
    other = tmp_path / 'modules' / 'B_Good' / 'module.json'
    other.write_text(json.dumps({**json.loads(other.read_text()), 'translator_version': '0.0.1'}))
    assert run_all(tmp_path, install) == scr2native.EXIT_OK
    rows = results(tmp_path)
    assert rows['A_Good'][7] == 'translated' and rows['B_Good'][7] == 'translated'
    assert json.loads(manifest.read_text()) == old


def test_all_stops_after_a_tool_failure_and_records_the_rest_as_not_run(tmp_path, monkeypatch, capsys):
    def broken(*a):
        raise scr2native.Stop(scr2native.EXIT_TOOL, 'docker is not installed; the lift runs pcrecomp')
    monkeypatch.setattr(scr2native, 'lift', broken)
    install = make_install(tmp_path, {'A_Good': make_dll(), 'B_Good': make_dll()})
    assert run_all(tmp_path, install) == scr2native.EXIT_TOOL
    rows = results(tmp_path)
    assert rows['A_Good'][7].startswith('failed (exit 3): docker is not installed')
    assert rows['B_Good'][7] == 'not run'
    assert 'scripts, 0 modules' in capsys.readouterr().out


def test_all_needs_an_install_with_scripts_and_excludes_a_single_script(tmp_path):
    assert run_all(tmp_path, tmp_path / 'nowhere') == scr2native.EXIT_INPUT
    empty = make_install(tmp_path, {})
    assert run_all(tmp_path, empty) == scr2native.EXIT_INPUT
    for argv in ([], ['X.scr', '--all', str(tmp_path)], ['--all', str(tmp_path), '--base', '0x30000000']):
        with pytest.raises(SystemExit) as stop:
            scr2native.parse_args(argv + ['-o', str(tmp_path)])
        assert stop.value.code == 2


def test_the_function_count_of_a_built_module_is_its_local_lifted_symbols(monkeypatch):
    nm = ('0000000000001000 T _MissionSpecEntry\n0000000000001010 t _sub_60001000\n'
          '0000000000001020 t _sub_6000100C\n0000000000001030 t _sub_nothex\n0000000000001040 U _memcpy\n')
    monkeypatch.setattr(scr2native, 'run', lambda cmd, *a, **k: nm)
    assert scr2native.library_functions('lib') == 2


# --- the host baseline list ----------------------------------------------------------------------

@pytest.mark.skipif(not (platform.system() == 'Darwin' and platform.machine() == 'arm64'
                         and shutil.which('cc')), reason='the committed baseline is arm64 macOS')
@pytest.mark.skipif(not os.path.isfile(os.path.join(scr2native.paths.WORK, 'lifter-hle', 'pcrecomp', 'runtime',
                                                    'recomp32', 'recomp_types.h')),
                    reason='no pcrecomp clone')
def test_the_committed_baseline_is_what_the_generator_makes_on_this_host():
    proc = subprocess.run([sys.executable, '-I', str(TOOL / 'gen_baseline.py'), '--check'],
                          capture_output=True, text=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr


# --- the oracle mission ------------------------------------------------------------------------

@retail
@need_oracle
def test_the_oracle_mission_reads_passes_the_import_check_and_rebases():
    data, dll, exports, imports = scr2native.read_scr(str(ORACLE))
    assert set(scr2native.EXPORTS) <= set(exports)
    scr2native.check_imports('OracleSkirmish', imports, scr2native.read_list(scr2native.gen_allowlist.ALLOWLIST))
    assert scr2native.guest_base(str(ORACLE), None) == 0x60000000 + 0x100000 * sorted(
        (n for n in os.listdir(ORACLE.parent) if n.lower().endswith('.scr')),
        key=lambda n: n.encode()).index('OracleSkirmish.scr')
    moved = dll.rebased(0x60000000)
    delta = 0x60000000 - dll.image_base
    for rva in dll.relocations()[:50]:
        before = struct.unpack('<I', dll.read(rva, 4))[0]
        assert struct.unpack('<I', moved.read(rva, 4))[0] == (before + delta) & 0xFFFFFFFF
    seeds = moved.code_pointer_seeds()
    assert seeds and all(0x60000000 <= s < 0x60000000 + moved.size_of_image for s in seeds)


@retail
@need_oracle
@pytest.mark.skipif(os.environ.get('SFCOP_SCR2NATIVE_LIFT') != '1',
                    reason='lifts with Docker for minutes; set SFCOP_SCR2NATIVE_LIFT=1')
def test_the_oracle_mission_translates_to_a_module_the_loader_accepts(tmp_path):
    proc = subprocess.run([sys.executable, '-I', str(TOOL / 'scr2native.py'), str(ORACLE),
                           '-o', str(tmp_path / 'modules'), '--work', str(tmp_path / 'work')],
                          capture_output=True, text=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    module = tmp_path / 'modules' / 'OracleSkirmish'
    assert (module / ('OracleSkirmish' + scr2native.library_suffix())).is_file()
    assert json.loads((module / 'module.json').read_text())['name'] == 'OracleSkirmish'
