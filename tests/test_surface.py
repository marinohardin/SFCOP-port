"""The Win32 surface contract: src/hle/surface.tsv, its generator and the COM layouts in src/hle/com/.

Most tests read only committed files. The tests marked with `retail` read the original exe and
are skipped where it is absent.
"""
import os
import pathlib
import shutil
import struct
import subprocess
import sys

import pytest

ROOT = pathlib.Path(__file__).resolve().parent.parent
HLE = ROOT / 'src' / 'hle'
COM = HLE / 'com'
sys.path.insert(0, str(HLE))
import gen_surface  # noqa: E402


def _exe():
    try:
        path, _ = gen_surface.locate_exe()
    except (OSError, ImportError):
        return None
    return path if os.path.exists(path) else None


EXE = _exe()
retail = pytest.mark.skipif(EXE is None, reason='the original exe is not on this machine')

# Imports per DLL in the exe's import table, counted by hand from the P0-4 import listing.
IMPORTS_PER_MODULE = {
    'KERNEL32.DLL': 169, 'USER32.DLL': 74, 'WSOCK32.DLL': 28, 'MSS32.DLL': 26, 'GDI32.DLL': 22,
    'ADVAPI32.DLL': 9, 'DBGHELP.DLL': 9, 'COMDLG32.DLL': 5, 'OLE32.DLL': 3, 'WINMM.DLL': 3,
    'DDRAW.DLL': 3, 'DSOUND.DLL': 1, 'DINPUT.DLL': 1,
}

# Slot counts of each interface's public definition (DirectX 6 SDK order, as in Wine's headers).
SLOTS = {
    'IDirectDraw': 23, 'IDirectDraw2': 24, 'IDirectDraw4': 28, 'IDirectDrawSurface3': 40,
    'IDirectDrawSurface4': 45, 'IDirectDrawClipper': 9, 'IDirectDrawPalette': 7,
    'IDirect3D3': 12, 'IDirect3DDevice3': 42, 'IDirect3DTexture2': 6, 'IDirect3DViewport3': 21,
    'IDirectInputA': 8, 'IDirectInputDeviceA': 18,
    'IDirectSound': 11, 'IDirectSoundBuffer': 21, 'IDirectSound3DBuffer': 21,
    'IDirectSound3DListener': 18,
    'IDirectPlay4A': 53, 'IDirectPlayLobby3A': 19,
}

# Slots the game is seen calling, with the method the binary's own evidence gives them: error
# strings next to the DirectPlay calls (analysis/netcode/directplay.md) and the call census of
# the lifter spike (spike/lifter/results/dx_inventory.md).
KNOWN_SLOTS = [
    ('IDirectDraw4', 4, 'CreateClipper'), ('IDirectDraw4', 5, 'CreatePalette'),
    ('IDirectDraw4', 6, 'CreateSurface'), ('IDirectDraw4', 10, 'FlipToGDISurface'),
    ('IDirectDraw4', 12, 'GetDisplayMode'), ('IDirectDraw4', 19, 'RestoreDisplayMode'),
    ('IDirectDraw4', 20, 'SetCooperativeLevel'), ('IDirectDraw4', 21, 'SetDisplayMode'),
    ('IDirectDrawSurface4', 5, 'Blt'), ('IDirectDrawSurface4', 20, 'GetPalette'),
    ('IDirectDrawSurface4', 24, 'IsLost'), ('IDirectDrawSurface4', 25, 'Lock'),
    ('IDirectDrawSurface4', 27, 'Restore'), ('IDirectDrawSurface4', 32, 'Unlock'),
    ('IDirect3D3', 6, 'CreateViewport'),
    ('IDirect3DDevice3', 3, 'GetCaps'), ('IDirect3DDevice3', 8, 'EnumTextureFormats'),
    ('IDirect3DDevice3', 9, 'BeginScene'), ('IDirect3DDevice3', 10, 'EndScene'),
    ('IDirect3DDevice3', 22, 'SetRenderState'), ('IDirect3DDevice3', 28, 'DrawPrimitive'),
    ('IDirect3DDevice3', 38, 'SetTexture'), ('IDirect3DDevice3', 40, 'SetTextureStageState'),
    ('IDirect3DDevice3', 41, 'ValidateDevice'),
    ('IDirectInputA', 3, 'CreateDevice'),
    ('IDirectInputDeviceA', 7, 'Acquire'), ('IDirectInputDeviceA', 8, 'Unacquire'),
    ('IDirectInputDeviceA', 11, 'SetDataFormat'), ('IDirectInputDeviceA', 13, 'SetCooperativeLevel'),
    ('IDirectSoundBuffer', 4, 'GetCurrentPosition'), ('IDirectSoundBuffer', 9, 'GetStatus'),
    ('IDirectSoundBuffer', 11, 'Lock'), ('IDirectSoundBuffer', 12, 'Play'),
    ('IDirectSoundBuffer', 13, 'SetCurrentPosition'), ('IDirectSoundBuffer', 15, 'SetVolume'),
    ('IDirectSoundBuffer', 16, 'SetPan'), ('IDirectSoundBuffer', 18, 'Stop'),
    ('IDirectSoundBuffer', 19, 'Unlock'), ('IDirectSoundBuffer', 20, 'Restore'),
    ('IDirectSound3DBuffer', 14, 'SetConeOrientation'),
    ('IDirectSound3DBuffer', 15, 'SetConeOutsideVolume'),
    ('IDirectSound3DBuffer', 19, 'SetPosition'), ('IDirectSound3DBuffer', 20, 'SetVelocity'),
    ('IDirectSound3DListener', 11, 'SetDistanceFactor'),
    ('IDirectSound3DListener', 13, 'SetOrientation'),
    ('IDirectSound3DListener', 14, 'SetPosition'), ('IDirectSound3DListener', 16, 'SetVelocity'),
    ('IDirectPlay4A', 5, 'CreateGroup'), ('IDirectPlay4A', 6, 'CreatePlayer'),
    ('IDirectPlay4A', 9, 'DestroyPlayer'), ('IDirectPlay4A', 13, 'EnumSessions'),
    ('IDirectPlay4A', 18, 'GetPlayerAddress'), ('IDirectPlay4A', 24, 'Open'),
    ('IDirectPlay4A', 26, 'Send'), ('IDirectPlay4A', 35, 'EnumConnections'),
    ('IDirectPlay4A', 38, 'InitializeConnection'), ('IDirectPlay4A', 49, 'SendEx'),
    ('IDirectPlay4A', 50, 'GetMessageQueue'), ('IDirectPlay4A', 51, 'CancelMessage'),
    ('IDirectPlayLobby3A', 5, 'EnumAddress'), ('IDirectPlayLobby3A', 8, 'GetConnectionSettings'),
    ('IDirectPlayLobby3A', 12, 'SetConnectionSettings'),
    ('IDirectPlayLobby3A', 14, 'CreateCompoundAddress'), ('IDirectPlayLobby3A', 15, 'ConnectEx'),
    ('IDirectPlayLobby3A', 18, 'WaitForConnectionSettings'),
]

# GUIDs the exe holds in .rdata, by virtual address (found by searching it for the public values).
GUIDS_IN_EXE = {
    'IID_IDirectDraw2': (0x007BB1D8, '{B3A6F3E0-2B43-11CF-A2DE-00AA00B93356}'),
    'IID_IDirectDraw4': (0x007BB1C8, '{9C59509A-39BD-11D1-8C4A-00C04FD930C5}'),
    'IID_IDirectDrawSurface3': (0x007BB1B8, '{DA044E00-69B2-11D0-A1D5-00AA00B8DFBB}'),
    'IID_IDirectDrawSurface4': (0x007BB1A8, '{0B2B8630-AD35-11D0-8EA6-00609797EA5B}'),
    'IID_IDirect3D3': (0x007BB198, '{BB223240-E72B-11D0-A9B4-00AA00C0993E}'),
    'IID_IDirect3DTexture2': (0x007BB158, '{93281502-8CF8-11D0-89AB-00A0C9054129}'),
    'IID_IDirectSound3DListener': (0x007BB048, '{279AFA84-4981-11CE-A521-0020AF0BE560}'),
    'IID_IDirectSound3DBuffer': (0x007BB038, '{279AFA86-4981-11CE-A521-0020AF0BE560}'),
    'IID_IDirectPlay4A': (0x007BB128, '{0AB1C531-4745-11D1-A7A1-0000F803ABFC}'),
    'CLSID_DirectPlay': (0x007BB118, '{D1EB6D20-8923-11D0-9D97-00A0C90A43CB}'),
    'IID_IDirectPlayLobby3A': (0x007BB0C8, '{2DB72491-652C-11D1-A7A8-0000F803ABFC}'),
    'CLSID_DirectPlayLobby': (0x007BB0B8, '{2FE8F810-B2A5-11D0-A787-0000F803ABFC}'),
}


def guid_bytes(text):
    """'{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}' in memory order."""
    a, b, c, d, e = text.strip('{}').split('-')
    return struct.pack('<IHH', int(a, 16), int(b, 16), int(c, 16)) + bytes.fromhex(d + e)


def surface_rows():
    lines = (HLE / 'surface.tsv').read_text().splitlines()
    return [dict(zip(gen_surface.COLUMNS, line.split('\t'))) for line in lines[1:]], lines[0]


def headers():
    return {iface: (methods, guids) for iface, methods, guids, _ in gen_surface.com_headers()}


# ---------------------------------------------------------------- the COM layouts

def test_every_interface_the_game_obtains_has_a_layout_with_its_public_slot_count():
    found = headers()
    assert sorted(found) == sorted(SLOTS)
    for iface, n in SLOTS.items():
        methods, _ = found[iface]
        assert [s for s, _, _ in methods] == list(range(n)), iface


def test_every_layout_starts_with_iunknown():
    for iface, (methods, _) in headers().items():
        assert [(m, b) for _, m, b in methods[:3]] == [
            ('QueryInterface', 12), ('AddRef', 4), ('Release', 4)], iface


def test_slots_the_game_calls_hold_the_methods_its_evidence_names():
    found = headers()
    for iface, slot, method in KNOWN_SLOTS:
        assert found[iface][0][slot][1] == method, (iface, slot)


def test_layout_guids_are_the_public_values():
    found = {}
    for _, guids in headers().values():
        found.update(guids)
    for name, (_, text) in GUIDS_IN_EXE.items():
        assert found[name] == guid_bytes(text), name
    for iface in SLOTS:
        assert 'IID_' + iface in found, iface


def test_a_method_line_out_of_slot_order_is_refused(tmp_path):
    bad = tmp_path / 'IBroken.h'
    bad.write_text('typedef struct IBrokenVtbl {\n'
                   '    /*  0 */ HRESULT (SFCOP_STDCALL *QueryInterface)(IBroken *self, REFIID riid, void **object);\n'
                   '    /*  2 */ ULONG (SFCOP_STDCALL *Release)(IBroken *self);\n'
                   '} IBrokenVtbl;\n')
    with pytest.raises(ValueError, match='slot 2 out of order'):
        gen_surface.parse_com_header(str(bad))


def test_a_wrapped_method_line_is_refused(tmp_path):
    bad = tmp_path / 'IBroken.h'
    bad.write_text('typedef struct IBrokenVtbl {\n'
                   '    /*  0 */ HRESULT (SFCOP_STDCALL *QueryInterface)(IBroken *self,\n'
                   '                                                 REFIID riid, void **object);\n'
                   '} IBrokenVtbl;\n')
    with pytest.raises(ValueError, match='one-line form'):
        gen_surface.parse_com_header(str(bad))


def test_an_eight_byte_value_parameter_is_refused(tmp_path):
    bad = tmp_path / 'IBroken.h'
    bad.write_text('typedef struct IBrokenVtbl {\n'
                   '    /*  0 */ HRESULT (SFCOP_STDCALL *Seek)(IBroken *self, double position);\n'
                   '} IBrokenVtbl;\n')
    with pytest.raises(ValueError, match='8-byte'):
        gen_surface.parse_com_header(str(bad))


@pytest.mark.skipif(shutil.which('clang') is None, reason='no clang on this machine')
@pytest.mark.parametrize('target,lang', [
    ('i386-pc-windows-msvc', 'c'), ('i386-pc-windows-msvc', 'c++'), (None, 'c'), (None, 'c++')])
def test_layouts_compile_as_c_and_cpp_and_have_4_byte_slots_on_the_guest(tmp_path, target, lang):
    # The guest target checks SFCOP_COM_ASSERT_SLOTS at 4 bytes a slot; the host build checks
    # that the headers stand alone without any Windows header.
    src = tmp_path / ('all.' + ('c' if lang == 'c' else 'cpp'))
    src.write_text(''.join('#include "%s.h"\n' % i for i in sorted(SLOTS)))
    cmd = ['clang', '-fsyntax-only', '-Wall', '-Wextra', '-Werror', '-I', str(COM),
           '-std=c11' if lang == 'c' else '-std=c++17', '-x', lang, str(src)]
    if target:
        cmd.insert(1, '--target=' + target)
    run = subprocess.run(cmd, capture_output=True, text=True)
    assert run.returncode == 0, run.stderr


# ---------------------------------------------------------------- surface.tsv

def test_surface_has_the_contract_columns_and_one_row_per_entry():
    rows, header = surface_rows()
    assert header.split('\t') == ['module', 'name', 'ordinal_or_slot', 'convention',
                                  'stack_bytes', 'status', 'evidence', 'notes']
    keys = [(r['module'], r['name']) for r in rows]
    assert len(keys) == len(set(keys))
    assert len(rows) == 353 + sum(SLOTS.values())


def test_no_cell_says_unknown_and_only_notes_may_be_empty():
    rows, _ = surface_rows()
    for r in rows:
        for c, v in r.items():
            assert 'unknown' not in v.lower(), (r['module'], r['name'], c)
            if c != 'notes':
                assert v != '', (r['module'], r['name'], c)


def test_conventions_and_statuses_come_from_the_vocabulary():
    rows, _ = surface_rows()
    for r in rows:
        assert r['convention'] in {'stdcall', 'cdecl', 'thiscall', 'varargs'}, r
        assert r['status'] in {'implemented', 'stub', 'implemented-spike', 'stub-spike',
                               'unused', 'todo'}, r


def test_stack_bytes_are_whole_arguments_and_zero_when_the_caller_pops():
    rows, _ = surface_rows()
    for r in rows:
        n = int(r['stack_bytes'])
        assert n % 4 == 0 and 0 <= n <= 64, r
        if r['convention'] in ('cdecl', 'varargs'):
            assert n == 0, r
        if r['module'] == 'MSS32.DLL':
            assert r['name'].endswith('@%d' % n), r


def test_imports_per_module_match_the_exe_listing():
    rows, _ = surface_rows()
    counts = {}
    for r in rows:
        if r['module'].endswith('.DLL'):
            counts[r['module']] = counts.get(r['module'], 0) + 1
    assert counts == IMPORTS_PER_MODULE


def test_known_import_signatures():
    rows, _ = surface_rows()
    got = {(r['module'], r['name']): (r['ordinal_or_slot'], r['convention'], r['stack_bytes'])
           for r in rows}
    assert got[('KERNEL32.DLL', 'CreateFileA')] == ('-', 'stdcall', '28')
    assert got[('KERNEL32.DLL', 'GetTickCount')] == ('-', 'stdcall', '0')
    assert got[('USER32.DLL', 'wsprintfA')] == ('-', 'varargs', '0')
    assert got[('USER32.DLL', 'PeekMessageA')] == ('-', 'stdcall', '20')
    assert got[('WSOCK32.DLL', 'socket')] == ('23', 'stdcall', '12')
    assert got[('WSOCK32.DLL', 'WSAStartup')] == ('115', 'stdcall', '8')
    assert got[('WSOCK32.DLL', '__WSAFDIsSet')] == ('151', 'stdcall', '8')
    assert got[('WSOCK32.DLL', 'recvfrom')] == ('17', 'stdcall', '24')
    assert got[('DSOUND.DLL', 'DirectSoundCreate')] == ('1', 'stdcall', '12')
    assert got[('DINPUT.DLL', 'DirectInputCreateA')] == ('-', 'stdcall', '16')
    assert got[('DDRAW.DLL', 'DirectDrawCreate')] == ('-', 'stdcall', '12')
    assert got[('OLE32.DLL', 'CoCreateInstance')] == ('-', 'stdcall', '20')
    assert got[('DBGHELP.DLL', 'StackWalk')] == ('-', 'stdcall', '36')
    assert got[('MSS32.DLL', '_AIL_startup@0')] == ('-', 'stdcall', '0')


def test_only_wsock32_and_dsound_are_imported_by_ordinal():
    rows, _ = surface_rows()
    for r in rows:
        if r['module'].endswith('.DLL'):
            by_ordinal = r['ordinal_or_slot'] != '-'
            assert by_ordinal == (r['module'] in ('WSOCK32.DLL', 'DSOUND.DLL')), r


def test_com_rows_are_the_layouts_slot_for_slot():
    rows, _ = surface_rows()
    com = [r for r in rows if not r['module'].endswith('.DLL')]
    assert all(r['convention'] == 'stdcall' for r in com)
    tsv = [(r['module'], int(r['ordinal_or_slot']), r['name'], int(r['stack_bytes'])) for r in com]
    want = [(iface, s, m, b) for iface, (methods, _) in sorted(headers().items())
            for s, m, b in methods]
    assert sorted(tsv) == sorted(want)


def test_evidence_is_an_address_in_the_game_and_every_import_has_one():
    rows, _ = surface_rows()
    for r in rows:
        if r['evidence'] == '-':
            # A COM method no call site has been attributed to yet.
            assert not r['module'].endswith('.DLL'), r
            continue
        va = int(r['evidence'], 16)
        assert r['evidence'] == '0x%08X' % va
        assert 0x00401000 <= va < 0x00883000, r


def test_directplay_rows_follow_the_inventory():
    rows, _ = surface_rows()
    status = {(r['module'], r['ordinal_or_slot']): r['status'] for r in rows}
    # analysis/netcode/directplay.md: 28 IDirectPlay4A and 7 IDirectPlayLobby3A methods are called.
    used4 = {2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 13, 14, 18, 20, 21, 22, 24, 25, 26, 29, 30, 31, 35,
             38, 46, 49, 50, 51}
    used3 = {2, 5, 8, 12, 14, 15, 18}
    for slot in range(53):
        assert (status[('IDirectPlay4A', str(slot))] != 'unused') == (slot in used4), slot
    for slot in range(19):
        assert (status[('IDirectPlayLobby3A', str(slot))] != 'unused') == (slot in used3), slot


# ---------------------------------------------------------------- the generator

def tiny_pe():
    """A PE32 image with one DLL, one import by name and one by ordinal, a direct call to the
    first and a call to a jump thunk for the second. Layout written out by hand."""
    img = bytearray(0x600)
    img[0:2] = b'MZ'
    struct.pack_into('<I', img, 0x3C, 0x40)
    img[0x40:0x44] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', img, 0x44, 0x14C, 2, 0, 0, 0, 0xE0, 0x102)
    opt = 0x58
    struct.pack_into('<H', img, opt, 0x10B)
    struct.pack_into('<I', img, opt + 28, 0x00400000)
    struct.pack_into('<I', img, opt + 92, 16)
    struct.pack_into('<II', img, opt + 96 + 8, 0x2000, 0x28)          # import directory
    sec = opt + 0xE0
    for i, (name, va, raw) in enumerate(((b'.text', 0x1000, 0x200), (b'.rdata', 0x2000, 0x400))):
        o = sec + 40 * i
        img[o:o + 8] = name.ljust(8, b'\0')
        struct.pack_into('<IIII', img, o + 8, 0x200, va, 0x200, raw)
    rd = 0x400                                                         # .rdata at rva 0x2000
    struct.pack_into('<IIIII', img, rd, 0x2040, 0, 0, 0x2080, 0x2060)
    struct.pack_into('<III', img, rd + 0x40, 0x2090, 0x80000017, 0)    # lookup table
    struct.pack_into('<III', img, rd + 0x60, 0x2090, 0x80000017, 0)    # IAT at 0x402060
    img[rd + 0x80:rd + 0x8B] = b'TEST32.dll\0'
    img[rd + 0x90:rd + 0x9A] = b'\0\0DoThing\0'
    tx = 0x200                                                         # .text at rva 0x1000
    img[tx:tx + 6] = bytes.fromhex('ff1560204000')                     # 0x401000 call [0x402060]
    img[tx + 6:tx + 11] = bytes.fromhex('e805000000')                  # 0x401006 call 0x401010
    img[tx + 0x10:tx + 0x16] = bytes.fromhex('ff2564204000')           # 0x401010 jmp [0x402064]
    return bytes(img)


def test_the_import_reader_lists_names_and_ordinals_with_their_slots():
    pe = gen_surface.Pe(tiny_pe())
    assert pe.imports() == [('TEST32.DLL', 'DoThing', None, 0x00402060),
                            ('TEST32.DLL', None, 23, 0x00402064)]


def test_evidence_is_a_direct_call_or_the_call_of_a_jump_thunk():
    pe = gen_surface.Pe(tiny_pe())
    assert gen_surface.import_evidence(pe, pe.imports()) == {0x00402060: 0x00401000,
                                                             0x00402064: 0x00401006}


def test_rows_keep_status_and_notes_and_new_rows_start_as_todo():
    pe = gen_surface.Pe(tiny_pe())
    imports = pe.imports()
    sigs = gen_surface.parse_signatures('TEST32.DLL DoThing 8\nTEST32.DLL #23 OtherThing 4\n')
    kept = {('TEST32.DLL', 'DoThing'): {'status': 'stub-spike', 'notes': 'returns 0',
                                        'evidence': '0x00400000'}}
    rows = gen_surface.build_rows(imports, gen_surface.import_evidence(pe, imports), [], kept,
                                  sigs)
    assert rows == [
        {'module': 'TEST32.DLL', 'name': 'DoThing', 'ordinal_or_slot': '-',
         'convention': 'stdcall', 'stack_bytes': '8', 'status': 'stub-spike',
         'evidence': '0x00401000', 'notes': 'returns 0'},
        {'module': 'TEST32.DLL', 'name': 'OtherThing', 'ordinal_or_slot': '23',
         'convention': 'stdcall', 'stack_bytes': '4', 'status': 'todo',
         'evidence': '0x00401006', 'notes': ''},
    ]


def test_a_status_outside_the_vocabulary_is_refused():
    row = {'module': 'TEST32.DLL', 'name': 'DoThing', 'ordinal_or_slot': '-',
           'convention': 'stdcall', 'stack_bytes': '8', 'status': 'unknown',
           'evidence': '0x00401000', 'notes': ''}
    with pytest.raises(ValueError, match="status 'unknown'"):
        gen_surface.render([row])


def test_an_import_missing_from_the_signature_table_stops_the_generator():
    pe = gen_surface.Pe(tiny_pe())
    sigs = gen_surface.parse_signatures('TEST32.DLL #23 OtherThing 4\n')
    with pytest.raises(ValueError, match='DoThing: not in SIGNATURES'):
        gen_surface.build_rows(pe.imports(), {}, [], {}, sigs)


# ---------------------------------------------------------------- against the exe

@retail
def test_every_import_in_the_exe_has_exactly_one_row():
    with open(EXE, 'rb') as f:
        pe = gen_surface.Pe(f.read())
    by_ordinal = {(m, o): n for (m, n), (o, _, _) in gen_surface.parse_signatures().items()
                  if o is not None}
    want = sorted((m, n if n else by_ordinal[(m, o)]) for m, n, o, _ in pe.imports())
    rows, _ = surface_rows()
    got = sorted((r['module'], r['name']) for r in rows if r['module'].endswith('.DLL'))
    assert len(want) == 353
    assert got == want


@retail
def test_surface_is_what_the_generator_writes():
    assert gen_surface.generate() == (HLE / 'surface.tsv').read_text()


@retail
def test_layout_guids_are_the_bytes_the_exe_holds():
    with open(EXE, 'rb') as f:
        pe = gen_surface.Pe(f.read())
    found = {}
    for _, guids in headers().values():
        found.update(guids)
    for name, (va, _) in GUIDS_IN_EXE.items():
        o = pe.offset(va - pe.base)
        assert pe.data[o:o + 16] == found[name], name
