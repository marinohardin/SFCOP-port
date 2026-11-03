"""Unit tests for the delink spike's Python tools. Run: python3 -m pytest spike/delink/tests"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'tools'))

import candidates  # noqa: E402
import coffsyms  # noqa: E402
import extract_function  # noqa: E402
import pecoff  # noqa: E402
import regtrace  # noqa: E402
import validate  # noqa: E402

COMDAT = 0x1000
CODE = 0x60000020
# Invented names and code: the tests check object handling, not the game.
FINFO = '?gState@tExample@@0HA'
FUNC = '?Probe@tExample@@QAE_NXZ'


def write_two_function_object(path):
    """A compiled translation unit in miniature: the wanted function in a COMDAT section that
    reads and writes a static through two DIR32 relocations, and a second function it must
    not carry. The code is invented: mov edx,[x]; test; jz; lea; mov [x],eax; ...; ret."""
    wanted = bytes.fromhex('8b1500000000' '85d2' '7409' '8d4201' 'a300000000' 'b801000000' 'c3' '33c0c3' '90')
    other = bytes.fromhex('33c0c3')
    # Section symbols take indexes 0-3 (each has one aux record); then FUNC 4, FINFO 5, _other 6.
    pecoff.write_coff_object(path, [
        ('.text', CODE | COMDAT, wanted, [(2, 5, pecoff.IMAGE_REL_I386_DIR32), (14, 5, pecoff.IMAGE_REL_I386_DIR32)]),
        ('.text', CODE, other, []),
    ], [(FUNC, 0, 1, 2), (FINFO, 0, 0, 2), ('_other', 0, 2, 2)])
    return wanted


def test_a_written_object_reads_back_with_its_bytes_relocations_and_long_names(tmp_path):
    path = tmp_path / 'two.obj'
    wanted = write_two_function_object(path)
    sections, symbols = coffsyms.read_object(path.read_bytes())
    assert [(s[1], s[2], s[3]) for s in sections] == [('.text', 28, CODE | COMDAT), ('.text', 3, CODE)]
    assert sections[0][4] == wanted
    assert sections[0][5] == [(2, 'DIR32', FINFO), (14, 'DIR32', FINFO)]
    externs = [(name, secnum) for _, name, _, secnum, cls in symbols if cls == 2]
    assert externs == [(FUNC, 1), (FINFO, 0), ('_other', 2)]


def test_extracting_a_function_keeps_only_its_section_and_what_it_needs(tmp_path):
    src, dst = tmp_path / 'tu.obj', tmp_path / 'only.obj'
    wanted = write_two_function_object(src)
    extract_function.main(str(src), FUNC, str(dst))
    sections, symbols = coffsyms.read_object(dst.read_bytes())
    assert len(sections) == 1
    _, name, size, chars, raw, relocs = sections[0]
    assert (name, size, raw) == ('.text', 28, wanted)
    assert chars == CODE, 'the COMDAT flag is dropped'
    assert relocs == [(2, 'DIR32', FINFO), (14, 'DIR32', FINFO)]
    externs = [(name, secnum) for _, name, _, secnum, cls in symbols if cls == 2]
    assert externs == [(FUNC, 1), (FINFO, 0)]


def test_text_like_marks_values_whose_three_low_bytes_are_printable():
    assert validate.text_like(0x00434C4D)        # "MLC\0"
    assert validate.text_like(0x00544853)        # "SHT\0"
    assert not validate.text_like(0x00838FCC)    # a .data address
    assert not validate.text_like(0x007BBBE4)    # an .rdata address


MAP = '''\
 sfcop_replaced

 Preferred load address is 10000000

 Start         Length     Name                   Class
 0001:00000000 00394010H .text                   CODE
 0002:00000000 000005b8H .idata$5                DATA
 0002:000005c0 0009fc60H .rdata                  DATA
 0003:00000000 0044b000H .data                   DATA

  Address         Publics by Value              Rva+Base     Lib:Object

 0001:00393ff0       ?Probe@tExample@@QAE_NXZ   10394ff0 f   Probe_only.obj
 0002:000000e8       __imp__CreateFileA@28      103960e8     KERNEL32:KERNEL32.dll
'''


def test_the_link_map_gives_section_contributions_and_public_symbols(tmp_path):
    path = tmp_path / 'x.map'
    path.write_text(MAP)
    assert validate.contributions(str(path)) == {
        1: [(0, '.text')], 2: [(0, '.idata$5'), (0x5C0, '.rdata')], 3: [(0, '.data')]}
    assert validate.public_symbols(str(path)) == {
        FUNC: 0x10394FF0, '__imp__CreateFileA@28': 0x103960E8}


LOG_A = '''\
0024:trace:reg:NtOpenKeyEx (0x40,L"Software\\\\Wine",2000000,0x1000fc518)
0024:trace:reg:NtOpenKeyEx (0x50,L"run-a.exe\\\\OpenGL",2000000,0x1000fe7e0)
0024:trace:reg:NtCreateKey (0x60,L"Video\\\\{0a1b2c3d-0000-4000-8000-00000000abcd}\\\\0000",0,0x1)
0024:trace:reg:RegQueryValueExA (0x70,"DirectDrawFlags",(nil),0x1,0x2)
0024:trace:reg:NtOpenKeyEx (0x40,L"Software\\\\Wine",2000000,0x1000fc518)
'''
LOG_B = '''\
0030:trace:reg:NtOpenKeyEx (0x48,L"Software\\\\Wine",2000000,0x1000fc600)
0030:trace:reg:NtCreateKey (0x68,L"Video\\\\{99999999-1111-4222-8333-444444444444}\\\\0000",0,0x1)
0030:trace:reg:NtOpenKeyEx (0x58,L"run-b.exe\\\\OpenGL",2000000,0x1000fe800)
0030:trace:reg:RegQueryValueExW (0x78,"DirectDrawFlags",(nil),0x1,0x2)
'''


def test_registry_traces_compare_equal_across_exe_names_handles_and_session_guids(tmp_path):
    a, b = tmp_path / 'a.log', tmp_path / 'b.log'
    a.write_text(LOG_A)
    b.write_text(LOG_B)
    assert regtrace.names(str(a), 'run-a') == [
        'NtOpenKeyEx L"Software\\\\Wine"',
        'NtOpenKeyEx L"<exe>.exe\\\\OpenGL"',
        'NtCreateKey L"Video\\\\<session-guid>\\\\0000"',
        'RegQueryValueEx "DirectDrawFlags"',
    ]
    assert regtrace.main(str(a), str(b), 'run-a', 'run-b') == 0


def test_a_key_only_one_boot_reached_makes_the_comparison_fail(tmp_path):
    a, b = tmp_path / 'a.log', tmp_path / 'b.log'
    a.write_text(LOG_A + '0024:trace:reg:NtOpenKeyEx (0x40,L"SOFTWARE\\\\Example\\\\Later",2000000,0x1)\n')
    b.write_text(LOG_B)
    assert regtrace.main(str(a), str(b), 'run-a', 'run-b') == 1


def text(v):
    return candidates.verdict(v, [], lambda a, n: True, True)


def test_unrelocated_values_get_the_rule_that_explains_them():
    def data(v, *neighbours, relocated=()):
        return candidates.verdict(v, list(enumerate(neighbours)), lambda a, n: n not in relocated, False)

    assert data(0x00434C4D, 0, 0) == 'text'                          # "MLC"
    assert data(0x006E0028, 0x006C0075, 0) == 'utf16'                # L"(n" beside L"ul"
    assert data(0x0068FFFE, 0x00400004, 0x00680004) == 'small16'     # -2 and 0x68
    assert data(0x00810081, 0, 0) == 'small16'
    assert data(0x006058AA, 0xC0F586E0, 0x30DC7D62) == 'constants'  # among random constants
    assert data(0x005BA2E8, 0xA2E5001A, 0) == 'UNDECIDED'           # a zero neighbour
    assert data(0x004E804B, 0x005A805F, 0x80410044, relocated=(0x005A805F,)) == 'UNDECIDED'


def test_immediates_are_text_or_round_constants_or_undecided():
    assert text(0x00415052) == 'text'          # "RPA"
    assert text(0x00800000) == 'round'
    assert text(0x00989680) == 'round'         # 10,000,000
    assert text(0x0053AF10) == 'UNDECIDED'
