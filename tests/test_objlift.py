"""tools/objlift: placement, relocation and lifting of VC6 COFF objects (DESIGN §6.3, §7.2).

The fixtures are written by tools/objlift/fixtures/make_coff.py, not by a compiler, so every byte
and every expected value below is worked out by hand. The lift tests need pcrecomp's lifter: they
run it in-process when capstone imports, else in the lifter container, else they skip. The
retail test compiles DynamicAllocator.cpp with the pinned VC6 and lifts its real object.

Where each runs: everywhere, the CI runner included (it has no lift): the placement, relocation,
COMDAT, command-line and report tests, the exe-lift format fixtures and the golden-C structure
test. Only with pcrecomp and capstone or the lifter container: the two lift tests (the golden
comparison among them). Only with the install, docker and the toolchain image: the retail test.
A test that cannot run says why on stderr (`skip`) and in pytest's -rs summary.
"""
import csv
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOL = os.path.join(REPO, 'tools', 'objlift')
for _p in (TOOL, os.path.join(TOOL, 'fixtures'), REPO):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import make_coff as mc  # noqa: E402
import compile as objcompile  # noqa: E402
import objlift  # noqa: E402
import placement  # noqa: E402
from harness.lib import paths  # noqa: E402
from placement import Claim, InputObject, Placer, PlacementError, SymbolIndex  # noqa: E402

EXE_SECTIONS = [(0x00401000, 0x00795000), (0x00795000, 0x00838000), (0x00838000, 0x00C83000)]


def write(tmp_path, name, obj):
    p = tmp_path / name
    p.write_bytes(obj.build())
    return str(p)


def objects(tmp_path, **objs):
    return [InputObject(write(tmp_path, n + '.obj', o), f'Test/{n}.cpp') for n, o in objs.items()]


def place(objs, claims, known=None, code=(), **kw):
    index = SymbolIndex({k: {v} for k, v in (known or {}).items()}, set(code))
    placement._claimed_sections(objs, claims)
    return Placer(objs, claims, index, exe_sections=EXE_SECTIONS, **kw).place()


def dword(result, sec, off):
    return struct.unpack_from('<I', result.images[sec], off)[0]


def section(result, label):
    return next(s for s in result.sections if s.label == label)


# --- relocations -----------------------------------------------------------------------------------

def every_type_object():
    """_f at 0x00401000. Its fields, each against _target (the exe's, at 0x00839000):
    +1 REL32 field 0, +5 DIR32 field 4, +9 DIR32NB field 0, +13 SECTION field 0,
    +15 SECREL field 8, +19 ABSOLUTE field 0x11223344 (ignored)."""
    o = mc.Obj()
    code = (b'\xe8' + b'\0' * 4 + struct.pack('<I', 4) + b'\0' * 4 + b'\0\0'
            + struct.pack('<I', 8) + struct.pack('<I', 0x11223344) + b'\xc3')
    t = o.section('.text', code, mc.CODE | mc.COMDAT | mc.ALIGN_16, selection=mc.NODUPLICATES)
    o.define('_f', t, 0, function=True)
    for off, typ in ((1, mc.REL32), (5, mc.DIR32), (9, mc.DIR32NB), (13, mc.SECTION),
                     (15, mc.SECREL), (19, mc.ABSOLUTE)):
        o.reloc(t, off, '_target', typ)
    return o


def test_every_relocation_type_vc6_emits(tmp_path):
    objs = objects(tmp_path, every=every_type_object())
    r = place(objs, [Claim(0x00401000, 24, '_f', 'f', 'Test/every.cpp')],
              known={'_target': 0x00839000})
    t = section(r, 'every.obj:.text#1')
    assert (t.mode, t.va) == ('claimed', 0x00401000)
    img = r.images[t]
    assert dword(r, t, 1) == 0x00437FFB          # 0x00839000 - (0x00401001 + 4)
    assert dword(r, t, 5) == 0x00839004          # 4 + 0x00839000
    assert dword(r, t, 9) == 0x00439000          # 0x00839000 - 0x00400000
    assert struct.unpack_from('<H', img, 13)[0] == 3   # the exe's third section, .data
    assert dword(r, t, 15) == 0x00001008         # 8 + 0x00839000 - 0x00838000
    assert dword(r, t, 19) == 0x11223344
    assert [(off, typ) for _, off, typ, _, _, _ in r.relocations] == [
        (1, 'REL32'), (5, 'DIR32'), (9, 'DIR32NB'), (13, 'SECTION'), (15, 'SECREL')]


def test_section_and_secrel_need_the_exe_sections(tmp_path):
    objs = objects(tmp_path, every=every_type_object())
    claims = [Claim(0x00401000, 24, '_f', 'f', 'Test/every.cpp')]
    placement._claimed_sections(objs, claims)
    index = SymbolIndex({'_target': {0x00839000}})
    with pytest.raises(PlacementError, match=r'needs the exe\'s section table'):
        Placer(objs, claims, index).place()


def test_an_unknown_relocation_type_is_refused(tmp_path):
    o = mc.Obj()
    t = o.section('.text', b'\0' * 4 + b'\xc3', mc.CODE)
    o.define('_f', t, 0, function=True)
    o.reloc(t, 0, '_target', 0x09)                  # REL_I386_SEG12
    objs = objects(tmp_path, seg=o)
    with pytest.raises(PlacementError, match='relocation type 0x9 against _target'):
        place(objs, [Claim(0x00401000, 5, '_f', 'f', 'Test/seg.cpp')], known={'_target': 0x839000})


def test_an_unresolved_external_is_refused_with_its_name(tmp_path):
    o = mc.Obj()
    t = o.section('.text', b'\xe8\0\0\0\0\xc3', mc.CODE | mc.COMDAT, selection=mc.NODUPLICATES)
    o.define('_f', t, 0, function=True)
    o.reloc(t, 1, '?Nowhere@@YAXXZ', mc.REL32)
    objs = objects(tmp_path, missing=o)
    with pytest.raises(PlacementError, match=r'unresolved external symbol \?Nowhere@@YAXXZ '
                                             r'\(referenced from missing\.obj:\.text#1\)'):
        place(objs, [Claim(0x00401000, 6, '_f', 'f', 'Test/missing.cpp')])


# --- placement -------------------------------------------------------------------------------------

def test_two_functions_in_one_section_share_its_placement(tmp_path):
    o = mc.Obj()
    t = o.section('.text', b'\xc3' + b'\x90' * 15 + b'\xe8\0\0\0\0\xc3', mc.CODE | mc.ALIGN_16)
    o.define('_a', t, 0, function=True)
    o.define('_b', t, 16, function=True)
    o.reloc(t, 17, '_a', mc.REL32)                  # _b calls _a
    objs = objects(tmp_path, two=o)
    r = place(objs, [Claim(0x00500000, 1, '_a', 'a', 'Test/two.cpp'),
                     Claim(0x00500010, 6, '_b', 'b', 'Test/two.cpp')])
    sec = section(r, 'two.obj:.text#1')
    assert (sec.mode, sec.va) == ('claimed', 0x00500000)
    assert dword(r, sec, 17) == 0xFFFFFFEB          # 0x00500000 - (0x00500011 + 4)
    entries = objlift.entries_by_section(r, [])
    assert entries[sec] == [0x00500000, 0x00500010]


def test_two_claims_that_split_one_section_are_refused(tmp_path):
    o = mc.Obj()
    t = o.section('.text', b'\xc3' + b'\x90' * 15 + b'\xc3', mc.CODE)
    o.define('_a', t, 0, function=True)
    o.define('_b', t, 16, function=True)
    objs = objects(tmp_path, two=o)
    with pytest.raises(PlacementError, match='inconsistent addresses: 0x00500000 and 0x00500010'):
        place(objs, [Claim(0x00500000, 1, '_a', 'a', 'Test/two.cpp'),
                     Claim(0x00500020, 1, '_b', 'b', 'Test/two.cpp')])


def code_and_data_object():
    """_f calls a helper the exe does not have, reads a string the exe does not have, and
    increments a counter the exe does have; a .drectve and an unused function are dropped."""
    o = mc.Obj()
    o.section('.drectve', b'-defaultlib:LIBC ', mc.INFO | mc.ALIGN_1)
    # call _helper / mov eax, offset string / inc dword ptr [_gCount] / ret
    code = b'\xe8\0\0\0\0' + b'\xb8\0\0\0\0' + b'\xff\x05\0\0\0\0' + b'\xc3'
    t = o.section('.text', code, mc.CODE | mc.COMDAT | mc.ALIGN_16, selection=mc.NODUPLICATES)
    o.define('_f', t, 0, function=True)
    h = o.section('.text', b'\x33\xc0\xc3', mc.CODE | mc.COMDAT | mc.ALIGN_16, selection=mc.ANY)
    o.define('?helper@@YAHXZ', h, 0, function=True)
    s = o.section('.rdata', b'abc\0', mc.RDATA | mc.COMDAT | mc.ALIGN_4, selection=mc.ANY)
    o.define('??_C@_03FHKD@abc?$AA@', s, 0)
    d = o.section('.data', b'\0' * 8, mc.DATA | mc.ALIGN_4)
    o.define('_gOther', d, 0)
    o.define('_gCount', d, 4)
    u = o.section('.text', b'\xc3', mc.CODE | mc.COMDAT | mc.ALIGN_16, selection=mc.ANY)
    o.define('?unused@@YAXXZ', u, 0, function=True)
    o.reloc(t, 1, '?helper@@YAHXZ', mc.REL32)
    o.reloc(t, 6, '??_C@_03FHKD@abc?$AA@', mc.DIR32)
    o.reloc(t, 12, '_gCount', mc.DIR32)
    return o


def test_data_referenced_from_code_goes_to_the_exe_or_the_reserved_range(tmp_path):
    objs = objects(tmp_path, unit=code_and_data_object())
    r = place(objs, [Claim(0x00401000, 17, '_f', 'f', 'Test/unit.cpp')],
              known={'_gCount': 0x00839104})
    by = {s.label: s for s in r.sections}
    f, helper = by['unit.obj:.text#2'], by['unit.obj:.text#3']
    string, data = by['unit.obj:.rdata#4'], by['unit.obj:.data#5']
    assert (helper.mode, helper.va) == ('fresh', 0x0F000000)
    assert (string.mode, string.va) == ('fresh', 0x0F000004)   # after the code, 4-aligned
    assert (data.mode, data.va) == ('exe', 0x00839100)          # _gCount is at +4
    assert dword(r, f, 1) == 0x0EBFEFFB             # 0x0F000000 - (0x00401001 + 4)
    assert dword(r, f, 6) == 0x0F000004
    assert dword(r, f, 12) == 0x00839104
    assert r.reserved_end == 0x0F000008
    assert data not in r.images                     # the exe's copy is the one used
    assert r.images[string] == b'abc\0'
    dropped = dict(r.dropped)
    assert dropped['unit.obj:.drectve#1'] == 'never put in an image'
    assert dropped['unit.obj:.text#6'] == 'not referenced by claimed code'
    assert [s.label for s in r.code_sections()] == ['unit.obj:.text#2', 'unit.obj:.text#3']


def test_writable_data_the_exe_does_not_hold_is_refused(tmp_path):
    objs = objects(tmp_path, unit=code_and_data_object())
    with pytest.raises(PlacementError, match=r'unit\.obj:\.data#5 is writable and has no address '
                                             r'in the exe \(_gOther, _gCount\)'):
        place(objs, [Claim(0x00401000, 17, '_f', 'f', 'Test/unit.cpp')])


def test_data_the_exe_holds_apart_is_resolved_symbol_by_symbol(tmp_path):
    objs = objects(tmp_path, unit=code_and_data_object())
    r = place(objs, [Claim(0x00401000, 17, '_f', 'f', 'Test/unit.cpp')],
              known={'_gCount': 0x00839104, '_gOther': 0x00840000})
    data = section(r, 'unit.obj:.data#5')
    assert data.mode == 'exe' and data.va is None
    assert data.split == {'_gOther': 0x00840000, '_gCount': 0x00839104}
    assert dword(r, section(r, 'unit.obj:.text#2'), 12) == 0x00839104


def test_evidence_from_an_exact_function_names_a_static(tmp_path):
    o = mc.Obj()
    t = o.section('.text', b'\xa1\0\0\0\0\xc3', mc.CODE | mc.COMDAT, selection=mc.NODUPLICATES)
    o.define('_f', t, 0, function=True)
    d = o.section('.bss', b'\0' * 4, mc.BSS | mc.ALIGN_4)
    o.define('_s_value', d, 0, static=True)
    o.reloc(t, 1, '_s_value', mc.DIR32)
    objs = objects(tmp_path, ev=o)
    evidence = {(objs[0].path, '_s_value'): 0x00C80010}
    r = place(objs, [Claim(0x00401000, 6, '_f', 'f', 'Test/ev.cpp')], evidence=evidence)
    assert section(r, 'ev.obj:.bss#2').va == 0x00C80010
    assert dword(r, section(r, 'ev.obj:.text#1'), 1) == 0x00C80010


REAL_HALF = '__real@4@3ffe8000000000000000'     # VC6's COMDAT literal for 0.5


def literal_user_object(fn):
    """fld qword ptr [__real@...] / ret in `fn`; the literal is a COMDAT (selection any) of its
    own, as VC6 emits one in every unit that uses the constant."""
    o = mc.Obj()
    t = o.section('.text', b'\xdd\x05\0\0\0\0\xc3', mc.CODE | mc.COMDAT | mc.ALIGN_16,
                  selection=mc.NODUPLICATES)
    o.define(fn, t, 0, function=True)
    r = o.section('.rdata', struct.pack('<d', 0.5), mc.RDATA | mc.COMDAT | mc.ALIGN_4,
                  selection=mc.ANY)
    o.define(REAL_HALF, r, 0)
    o.reloc(t, 2, REAL_HALF, mc.DIR32)
    return o


def stub_relocation_check(monkeypatch, uses):
    """harness/lib/target.build, stubbed: every claimed function is exact, and its references
    are uses[its address] = [(referenced symbol, exe address)]."""
    from types import SimpleNamespace
    from harness.lib import target

    def build(address, size, obj, symbol, exe, index):
        return {'strict': True, 'relocs': [
            SimpleNamespace(symbol=name, exe_address=addr, status='ok', detail='')
            for name, addr in uses[address]]}
    monkeypatch.setattr(target, 'build', build)


def test_two_units_exact_functions_may_use_their_own_copy_of_a_literal(tmp_path, monkeypatch):
    # The original link kept the literal once per unit: a's code reads 0x007959cc, b's 0x00795bf4.
    objs = objects(tmp_path, a=literal_user_object('_fa'), b=literal_user_object('_fb'))
    claims = [Claim(0x00401000, 7, '_fa', 'fa', 'Test/a.cpp'),
              Claim(0x00402000, 7, '_fb', 'fb', 'Test/b.cpp')]
    stub_relocation_check(monkeypatch, {0x00401000: [(REAL_HALF, 0x007959cc)],
                                        0x00402000: [(REAL_HALF, 0x00795bf4)]})
    evidence, notes = placement.exact_function_evidence(claims, objs, None, None)
    assert evidence == {(objs[0].path, REAL_HALF): 0x007959cc,
                        (objs[1].path, REAL_HALF): 0x00795bf4}
    assert notes == [f'{REAL_HALF}: the exact functions of different units use different copies '
                     'of it; each unit\'s references keep its own']
    r = place(objs, claims, evidence=evidence)
    assert (section(r, 'a.obj:.rdata#2').mode, section(r, 'a.obj:.rdata#2').va) == ('exe', 0x007959cc)
    assert (section(r, 'b.obj:.rdata#2').mode, section(r, 'b.obj:.rdata#2').va) == ('exe', 0x00795bf4)
    assert dword(r, section(r, 'a.obj:.text#1'), 2) == 0x007959cc
    assert dword(r, section(r, 'b.obj:.text#1'), 2) == 0x00795bf4


def test_units_that_agree_on_a_literal_share_one_copy(tmp_path, monkeypatch):
    objs = objects(tmp_path, a=literal_user_object('_fa'), b=literal_user_object('_fb'))
    claims = [Claim(0x00401000, 7, '_fa', 'fa', 'Test/a.cpp'),
              Claim(0x00402000, 7, '_fb', 'fb', 'Test/b.cpp')]
    stub_relocation_check(monkeypatch, {0x00401000: [(REAL_HALF, 0x007959cc)],
                                        0x00402000: [(REAL_HALF, 0x007959cc)]})
    evidence, notes = placement.exact_function_evidence(claims, objs, None, None)
    assert evidence[(None, REAL_HALF)] == 0x007959cc and notes == []
    r = place(objs, claims, evidence=evidence)
    assert dict(r.dropped)['b.obj:.rdata#2'] == 'COMDAT duplicate (any)'
    assert dword(r, section(r, 'b.obj:.text#1'), 2) == 0x007959cc


def test_one_units_references_to_a_literal_must_agree(tmp_path, monkeypatch):
    objs = objects(tmp_path, a=literal_user_object('_fa'))
    claims = [Claim(0x00401000, 7, '_fa', 'fa', 'Test/a.cpp')]
    stub_relocation_check(monkeypatch, {0x00401000: [(REAL_HALF, 0x007959cc),
                                                     (REAL_HALF, 0x00795bf4)]})
    with pytest.raises(PlacementError, match=f'{re.escape(REAL_HALF)}: exact functions of a.obj '
                                             'use it at 0x007959cc and 0x00795bf4'):
        placement.exact_function_evidence(claims, objs, None, None)


def test_a_plain_external_used_at_two_addresses_is_refused(tmp_path, monkeypatch):
    # _gValue is no object's COMDAT: one name, one variable, so two addresses is an error.
    objs = objects(tmp_path, a=two_instruction_object())
    claims = [Claim(0x00401000, 6, '?Value@@YAHXZ', 'Value', 'Test/a.cpp'),
              Claim(0x00402000, 6, '?Value@@YAHXZ', 'Value', 'Test/a.cpp')]
    stub_relocation_check(monkeypatch, {0x00401000: [('_gValue', 0x00838010)],
                                        0x00402000: [('_gValue', 0x00838020)]})
    with pytest.raises(PlacementError, match='_gValue: exact functions use it at 0x00838010 and '
                                             '0x00838020'):
        placement.exact_function_evidence(claims, objs, None, None)


def global_initializer_object(body):
    """A unit with a global: VC6's dynamic initializer `_$E1` is a static function in a COMDAT
    code section of its own, and every such unit has one."""
    o = mc.Obj()
    t = o.section('.text', body, mc.CODE | mc.COMDAT | mc.ALIGN_16, selection=mc.NODUPLICATES)
    o.define('_$E1', t, 0, function=True, static=True)
    return o


def test_a_static_claim_is_found_in_its_own_units_object_only(tmp_path):
    # a.obj and b.obj both define a static _$E1; the claim for Test/b.cpp takes b's.
    objs = objects(tmp_path, a=global_initializer_object(b'\x90\xc3'),
                   b=global_initializer_object(b'\xc3'))
    r = place(objs, [Claim(0x0040ceae, 1, '_$E1', 'g_b (dynamic initializer)', 'Test/b.cpp')])
    assert [(s.label, s.mode, s.va) for s in r.sections] == [('b.obj:.text#1', 'claimed', 0x0040ceae)]
    assert dict(r.dropped)['a.obj:.text#1'] == 'not referenced by claimed code'
    # The object can be named by path, as objlift.py does from --obj.
    objs = objects(tmp_path, a=global_initializer_object(b'\x90\xc3'),
                   b=global_initializer_object(b'\xc3'))
    r = place(objs, [Claim(0x0040ceae, 2, '_$E1', 'g_a (dynamic initializer)', 'Test/x.cpp',
                           objs[0].path)])
    assert [(s.label, s.va) for s in r.sections] == [('a.obj:.text#1', 0x0040ceae)]


def test_a_static_claim_in_another_units_object_is_refused(tmp_path):
    objs = objects(tmp_path, a=global_initializer_object(b'\xc3'))
    with pytest.raises(PlacementError, match=r'0x0040ceae g_b \(dynamic initializer\): its symbol '
                                             r'_\$E1 is not defined in any input object'):
        place(objs, [Claim(0x0040ceae, 1, '_$E1', 'g_b (dynamic initializer)', 'Test/b.cpp')])


def test_the_reserved_range_has_a_limit(tmp_path):
    objs = objects(tmp_path, unit=code_and_data_object())
    with pytest.raises(PlacementError, match='need 0x8 bytes; the reserved guest range holds 0x4'):
        place(objs, [Claim(0x00401000, 17, '_f', 'f', 'Test/unit.cpp')],
              known={'_gCount': 0x00839104}, reserved=(0x0F000000, 4))


def test_duplicate_code_addresses_take_the_lowest_and_say_so(tmp_path):
    o = mc.Obj()
    t = o.section('.text', b'\xe8\0\0\0\0\xc3', mc.CODE | mc.COMDAT, selection=mc.NODUPLICATES)
    o.define('_f', t, 0, function=True)
    o.reloc(t, 1, '??1tObject@@UAE@XZ', mc.REL32)
    objs = objects(tmp_path, dup=o)
    index = SymbolIndex({'??1tObject@@UAE@XZ': {0x00500020, 0x00500010}}, {0x00500010, 0x00500020})
    claims = [Claim(0x00401000, 6, '_f', 'f', 'Test/dup.cpp')]
    placement._claimed_sections(objs, claims)
    r = Placer(objs, claims, index).place()
    assert dword(r, section(r, 'dup.obj:.text#1'), 1) == 0x000FF00B   # 0x00500010 - 0x00401005
    assert r.notes == ['??1tObject@@UAE@XZ has 2 addresses in the exe (0x00500010, 0x00500020), '
                       'copies of one function; 0x00500010 is used']


# --- COMDAT selection ------------------------------------------------------------------------------

def comdat_pair(sel_a, sel_b, body_a=b'\xc3', body_b=b'\xc3'):
    """Two objects; each defines ?dup@@YAXXZ in a COMDAT, with an associative section beside
    it. Only a.obj's _f is claimed; it calls ?dup."""
    objs = {}
    for name, sel, body, claimed in (('a', sel_a, body_a, True), ('b', sel_b, body_b, False)):
        o = mc.Obj()
        if claimed:
            t = o.section('.text', b'\xe8\0\0\0\0\xc3', mc.CODE | mc.COMDAT,
                          selection=mc.NODUPLICATES)
            o.define('_f', t, 0, function=True)
            o.reloc(t, 1, '?dup@@YAXXZ', mc.REL32)
        d = o.section('.text', body, mc.CODE | mc.COMDAT | mc.ALIGN_16, selection=sel)
        o.define('?dup@@YAXXZ', d, 0, function=True)
        o.section('.rdata', b'\x01\x02\x03\x04', mc.RDATA | mc.COMDAT | mc.ALIGN_4,
                  selection=mc.ASSOCIATIVE, associated=d.number)
        objs[name] = o
    return objs


def test_comdat_any_keeps_the_first_and_its_associative_section(tmp_path):
    objs = objects(tmp_path, **comdat_pair(mc.ANY, mc.ANY))
    r = place(objs, [Claim(0x00401000, 6, '_f', 'f', 'Test/a.cpp')])
    assert [(s.label, s.mode, s.va) for s in r.sections] == [
        ('a.obj:.text#1', 'claimed', 0x00401000),
        ('a.obj:.text#2', 'fresh', 0x0F000000),
        ('a.obj:.rdata#3', 'fresh', 0x0F000004)]
    dropped = dict(r.dropped)
    assert dropped['b.obj:.text#1'] == 'COMDAT duplicate (any)'
    assert dropped['b.obj:.rdata#2'] == 'not referenced by claimed code'


def test_comdat_largest_takes_the_larger_copy(tmp_path):
    objs = objects(tmp_path, **comdat_pair(mc.LARGEST, mc.LARGEST, b'\xc3', b'\x90\x90\xc3'))
    r = place(objs, [Claim(0x00401000, 6, '_f', 'f', 'Test/a.cpp')])
    dup = section(r, 'b.obj:.text#1')
    assert (dup.mode, dup.va) == ('fresh', 0x0F000000)
    assert dword(r, section(r, 'a.obj:.text#1'), 1) == 0x0EBFEFFB
    assert section(r, 'b.obj:.rdata#2').va == 0x0F000004
    assert 'a.obj:.text#2' in dict(r.dropped)


@pytest.mark.parametrize('sel_a, sel_b, body_b, message', [
    (mc.NODUPLICATES, mc.NODUPLICATES, b'\xc3', 'selection "no duplicates"'),
    (mc.SAME_SIZE, mc.SAME_SIZE, b'\x90\xc3', '"same size" differs \\(1 and 2 bytes\\)'),
    (mc.EXACT_MATCH, mc.EXACT_MATCH, b'\xcc', '"exact match" differs'),
])
def test_comdat_selection_refuses_what_a_linker_refuses(tmp_path, sel_a, sel_b, body_b, message):
    with pytest.raises(PlacementError, match=r'\?dup@@YAXXZ: COMDAT .*' + message):
        objects_ = objects(tmp_path, **comdat_pair(sel_a, sel_b, b'\xc3', body_b))
        place(objects_, [Claim(0x00401000, 6, '_f', 'f', 'Test/a.cpp')])


def test_a_plain_symbol_defined_twice_is_refused(tmp_path):
    a, b = mc.Obj(), mc.Obj()
    for o in (a, b):
        t = o.section('.text', b'\xc3', mc.CODE)
        o.define('_f', t, 0, function=True)
    with pytest.raises(PlacementError, match=r'_f is defined twice \(a\.obj:\.text#1 and '
                                             r'b\.obj:\.text#1\)'):
        place(objects(tmp_path, a=a, b=b), [Claim(0x00401000, 1, '_f', 'f', 'Test/a.cpp')])


def test_the_obj_argument_splits_at_the_first_equals_sign(tmp_path):
    (tmp_path / 'a=b.obj').write_bytes(b'')
    spec = str(tmp_path / 'a=b.obj')
    assert objlift.parse_obj_args([f'Test/unit.cpp={spec}']) == [('Test/unit.cpp', spec)]
    assert objlift.parse_obj_args([f'U={tmp_path}/a=b.obj']) == [('U', f'{tmp_path}/a=b.obj')]
    (tmp_path / 'plain.obj').write_bytes(b'')
    assert objlift.parse_obj_args([str(tmp_path / 'plain.obj')]) == [(None, str(tmp_path / 'plain.obj'))]


def test_comdat_copies_that_disagree_on_the_selection_kind_are_refused(tmp_path):
    objs = objects(tmp_path, **comdat_pair(mc.ANY, mc.LARGEST))
    with pytest.raises(PlacementError, match=r'\?dup@@YAXXZ: COMDAT copies disagree on the '
                                             r'selection kind'):
        place(objs, [Claim(0x00401000, 6, '_f', 'f', 'Test/a.cpp')])


# --- the exe lift's generated text -----------------------------------------------------------------
# objlift.py reads two files the exe lift (cmake/lifter/run_lift.py) writes; these fixtures are
# their current formats, so a format change fails here rather than in a build.

EXE_LIFT = os.path.join(TOOL, 'fixtures', 'exe_lift')


def test_read_lifted_names_reads_the_exe_lifts_function_header():
    assert objlift.read_lifted_names(EXE_LIFT) == {0x00401000, 0x0040103B, 0x006FC4D0}


def test_read_dispatch_reads_the_exe_lifts_table_and_entry():
    entries, entry_va = objlift.read_dispatch(EXE_LIFT)
    assert entries == [(0x00401000, 'sub_00401000'), (0x0040103B, 'sub_0040103B'),
                       (0x006FC4D0, 'sub_006FC4D0')]
    assert entry_va == 0x0073A2A0


def test_read_dispatch_names_a_file_without_the_entry_line(tmp_path):
    (tmp_path / 'recomp_dispatch.c').write_text('{ 0x00401000u, sub_00401000 },\n')
    with pytest.raises(PlacementError, match='no sfcop_entry_va line'):
        objlift.read_dispatch(str(tmp_path))


# --- the command line ------------------------------------------------------------------------------

FUNCTIONS_HEADER = ['address', 'size', 'name', 'mangled', 'demangled', 'name_source',
                    'translation_unit', 'tu_evidence', 'headers', 'is_library', 'library', 'kind',
                    'tags']


def cli_inputs(tmp_path, obj, rows, functions=()):
    """A manifest, a functions.csv and an empty SDK table for the command line."""
    write(tmp_path, 'unit.obj', obj)
    with open(tmp_path / 'manifest.tsv', 'w', newline='') as f:
        f.write('address\tsize\tsymbol\tsource\tmodes\n')
        for address, size, name in rows:
            f.write(f'0x{address:08x}\t{size}\t{name}\tTest/unit.cpp\tvc6\n')
    with open(tmp_path / 'functions.csv', 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(FUNCTIONS_HEADER)
        for address, size, name, mangled in functions:
            w.writerow([f'{address:08x}', size, name, mangled, '', 'test', '', '', '', '0', '',
                        'function', ''])
    (tmp_path / 'sdk.tsv').write_text('address\tname\n')
    (tmp_path / 'game').mkdir(exist_ok=True)
    return ['--obj', f'Test/unit.cpp={tmp_path / "unit.obj"}', '--manifest',
            str(tmp_path / 'manifest.tsv'), '--symbols', str(tmp_path / 'functions.csv'),
            '--sdk-symbols', str(tmp_path / 'sdk.tsv'), '--game', str(tmp_path / 'game')]


def two_instruction_object():
    """mov eax, dword ptr [_gValue] / ret, claimed at 0x00401000; _gValue is the exe's
    0x00838010."""
    o = mc.Obj()
    t = o.section('.text', b'\xa1\0\0\0\0\xc3', mc.CODE | mc.COMDAT | mc.ALIGN_16,
                  selection=mc.NODUPLICATES)
    o.define('?Value@@YAHXZ', t, 0, function=True)
    o.reloc(t, 1, '_gValue', mc.DIR32)
    return o


TWO_FUNCTIONS = [(0x00401000, 6, 'Value', '?Value@@YAHXZ'), (0x00838010, 4, 'gValue', '_gValue')]


def test_the_command_line_places_and_reports_without_lifting(tmp_path, capsys):
    args = cli_inputs(tmp_path, two_instruction_object(), [(0x00401000, 6, 'Value()')],
                      TWO_FUNCTIONS)
    out = tmp_path / 'out'
    assert objlift.main(args + ['--out', str(out), '--no-lift']) == 0
    assert sorted(os.listdir(out)) == ['objlift_report.txt']
    text = (out / 'objlift_report.txt').read_text()
    assert '  0x00401000 ?Value@@YAHXZ (Value(), game/Test/unit.cpp)\n' in text
    assert '  unit.obj:.text#1+0x1 DIR32   _gValue -> 0x00838010 (exe symbol)\n' in text
    assert 'data image: empty\n' in text
    assert objlift.main(args + ['--out', str(out), '--no-lift', '--check']) == 0


def test_the_command_line_names_an_unresolved_external(tmp_path, capsys):
    args = cli_inputs(tmp_path, two_instruction_object(), [(0x00401000, 6, 'Value()')],
                      TWO_FUNCTIONS[:1])
    assert objlift.main(args + ['--out', str(tmp_path / 'out'), '--no-lift']) == 2
    assert 'unresolved external symbol _gValue' in capsys.readouterr().err


def test_a_claim_without_its_object_is_refused(tmp_path, capsys):
    args = cli_inputs(tmp_path, two_instruction_object(), [(0x00401000, 6, 'Value()')],
                      TWO_FUNCTIONS)
    args[1] = f'Test/other.cpp={tmp_path / "unit.obj"}'
    assert objlift.main(args + ['--out', str(tmp_path / 'out'), '--no-lift']) == 2
    assert 'no --obj for its unit Test/unit.cpp' in capsys.readouterr().err


# --- lifting ---------------------------------------------------------------------------------------

def skip(reason):
    """Skip, and say why on the real stderr too, so a log that hides pytest's skip summary
    still shows which lift-dependent test did not run."""
    print(f'test_objlift: SKIPPED: {reason}', file=sys.__stderr__, flush=True)
    pytest.skip(reason)


def _pcrecomp():
    d = os.environ.get('SFCOP_PCRECOMP_DIR') or os.path.join(paths.WORK, 'lifter-hle', 'pcrecomp')
    return d if os.path.isfile(os.path.join(d, 'tools', 'lift', 'lift32.py')) else None


def _lifter_image():
    if shutil.which('docker') is None:
        return None
    digest = hashlib.sha256(open(os.path.join(REPO, 'cmake', 'lifter', 'Dockerfile'), 'rb')
                            .read()).hexdigest()[:12]
    image = f'sfcop-lifter-py:{digest}'
    ok = subprocess.run(['docker', 'image', 'inspect', image], capture_output=True).returncode == 0
    return image if ok else None


def run_lift(tmp_path, args):
    """objlift.py with pcrecomp: in-process when capstone imports, else in the lifter container
    (no network, the repository and pcrecomp read-only). Returns (status, stderr)."""
    pcr = _pcrecomp()
    if pcr is None:
        skip('no pcrecomp clone (SFCOP_PCRECOMP_DIR, or work/lifter-hle/pcrecomp)')
    try:
        import capstone  # noqa: F401
        local = True
    except ImportError:
        local = False
    if local:
        return objlift.main(args + ['--pcrecomp', pcr]), ''
    image = _lifter_image()
    if image is None:
        skip('neither capstone nor the lifter container image (sfcop-lifter-py:<digest>) is available')
    inner = [a.replace(str(tmp_path), '/t') for a in args]
    p = subprocess.run(['docker', 'run', '--rm', '--network', 'none', '-v', f'{REPO}:/r:ro',
                        '-v', f'{pcr}:/p:ro', '-v', f'{tmp_path}:/t', image, 'python', '-I',
                        '/r/tools/objlift/objlift.py'] + inner + ['--pcrecomp', '/p'],
                       capture_output=True, text=True)
    return p.returncode, p.stderr


# The expected C of the two-instruction case is a committed golden file, produced by the tool
# (tools/objlift/fixtures/two_instruction.c). Two tests hold it: the lift test regenerates the body
# and compares (needs pcrecomp and capstone or the container), and the structure test below runs
# everywhere and checks the golden against the conventions written out by hand. To refresh it after a
# deliberate change of the lifter: SFCOP_UPDATE_GOLDEN=1 python3 -m pytest tests/test_objlift.py.
GOLDEN = os.path.join(TOOL, 'fixtures', 'two_instruction.c')


def golden():
    with open(GOLDEN) as f:
        return f.read()


def test_the_golden_c_has_the_lifting_conventions_written_by_hand():
    lines = golden().splitlines()
    assert lines[0] == '/* 0x00401000 claimed .text, game/Test/unit.cpp */'
    assert lines[1] == 'void sub_00401000(void) {' and lines[-1] == '}'
    # the fixed preamble of flag and register locals
    assert lines[2:9] == ['    int _fpu_cmp = 0;', '    uint32_t _cf = 0;', '    int _df = 1;',
                          '    uint32_t _flag_k = FK_NONE;', '    uint32_t _flag_a = 0, _flag_b = 0;',
                          '    uint32_t _itail_tgt = 0;', '    RECOMP_REGS_LOCALS']
    # the hook before RECOMP_ENTER, so the counter sees the caller; the index is the table's
    # constant, so a claim added elsewhere does not change this body
    assert ('    sfcop_guest_entered(sfcop_objlift_index_00401000); RECOMP_ENTER(0x00401000u);'
            in lines)
    # the load from the relocated field, the exe's _gValue at 0x00838010
    assert '    eax = MEM32(0x838010); /* 0x00401000: mov eax, dword ptr [0x838010] */' in lines
    # the return pops the return address and leaves through the register write-back
    assert any(l.startswith('    RECOMP_FLAGS_OUT(); esp += 4; { RECOMP_REGS_OUT(); return; }')
               for l in lines)
    assert sum(1 for l in lines if l.startswith('L_')) == 1




def test_a_two_instruction_function_lifts_to_the_expected_c(tmp_path):
    args = cli_inputs(tmp_path, two_instruction_object(), [(0x00401000, 6, 'Value()')],
                      TWO_FUNCTIONS)
    out = tmp_path / 'out'
    status, err = run_lift(tmp_path, args + ['--out', str(out)])
    assert status == 0, err
    code = (out / 'objlift_code_Test_unit.c').read_text()
    body = code[code.index('/* 0x00401000'):]
    body = body.rstrip('\n') + '\n'
    if os.environ.get('SFCOP_UPDATE_GOLDEN') == '1':
        with open(GOLDEN, 'w') as f:
            f.write(body)
    assert body == golden()
    table = (out / 'objlift_table.c').read_text()
    assert '    {0x00401000u, sub_00401000, "Value()", "Test/unit.cpp"},\n' in table
    assert 'const unsigned sfcop_guest_override_count = 1u;\n' in table
    assert 'const unsigned sfcop_objlift_index_00401000 = 0u;\n' in table
    assert 'const unsigned sfcop_guest_object_section_count = 0u;\n' in table
    assert (out / 'objlift_funcs.h').read_text().endswith('void sub_00401000(void);\n')
    # no --lifted: no exe-lift file to weaken a body in
    assert not [n for n in os.listdir(out) if n.startswith('objlift_weak')]


def test_fresh_code_lifts_under_its_reserved_address_and_loads_the_image(tmp_path):
    obj = code_and_data_object()
    args = cli_inputs(tmp_path, obj, [(0x00401000, 17, 'f()')],
                      [(0x00401000, 17, 'f', '_f'), (0x00839104, 4, 'gCount', '_gCount')])
    out = tmp_path / 'out'
    status, err = run_lift(tmp_path, args + ['--out', str(out)])
    assert status == 0, err
    code = (out / 'objlift_code_Test_unit.c').read_text()
    assert 'RECOMP_CALL(sub_0F000000); /* 0x00401000: call 0xf000000 */' in code
    assert ('SFCOP_GUEST_OBJECTS_LOAD(); sfcop_guest_entered(sfcop_objlift_index_00401000); '
            'RECOMP_ENTER(0x00401000u);') in code
    assert 'SFCOP_GUEST_OBJECTS_LOAD(); RECOMP_ENTER(0x0F000000u);' in code
    # The macro is defined in the guest override header, so the code file includes it.
    assert '#include "app/guest/override.h"\n' in code
    table = (out / 'objlift_table.c').read_text()
    assert ('static const uint8_t sfcop_guest_object_bytes_1[4] = {\n'
            '    0x61, 0x62, 0x63, 0x00,\n};\n') in table
    assert '    {0x0F000004u, 4u, sfcop_guest_object_bytes_1}, /* unit.obj:.rdata#4 */\n' in table
    assert 'const uint32_t sfcop_guest_object_size = 0x10000u;\n' in table
    # the fresh body is declared in the unit's own file; the claimed one too, beside recomp_funcs.h
    assert 'void sub_00401000(void);\nvoid sub_0F000000(void);\n' in code
    assert 'extern const unsigned sfcop_objlift_index_00401000;\n' in code


def test_the_lift_weakens_each_claimed_body_in_its_own_exe_lift_file(tmp_path):
    obj = code_and_data_object()
    args = cli_inputs(tmp_path, obj, [(0x00401000, 17, 'f()')],
                      [(0x00401000, 17, 'f', '_f'), (0x00839104, 4, 'gCount', '_gCount')])
    gen = fake_exe_lift(tmp_path / 'gen', [[0x00400000], [0x00401000, 0x00402000], []])
    out = tmp_path / 'out'
    status, err = run_lift(tmp_path, args + ['--out', str(out), '--lifted', str(gen)])
    assert status == 0, err
    assert (out / 'objlift_weak_0000.h').read_text() == WEAK_HEAD % '0000'
    # the fresh helper at 0x0F000000 has no exe-lift body to weaken
    assert (out / 'objlift_weak_0001.h').read_text() == (WEAK_HEAD % '0001'
                                                         + '#pragma weak sub_00401000\n')
    assert (out / 'objlift_weak_0002.h').read_text() == WEAK_HEAD % '0002'
    disp = (out / 'objlift_dispatch.c').read_text()
    assert '    { 0x00401000u, sub_00401000 },\n    { 0x00402000u, sub_00402000 },\n' \
           '    { 0x0F000000u, sub_0F000000 },\n};\n' in disp


# --- per-unit and per-file output -----------------------------------------------------------------
# These emit with bodies written by hand in place of pcrecomp's, so they run everywhere: what they
# hold is which file each body and each weak line goes to, and which files a change rewrites.

WEAK_HEAD = ('/* Generated by tools/objlift/objlift.py from the VC6 objects of the claimed units. */\n'
             '/* The bodies of recomp_%s.c that the lifted objects replace. */\n')


def fake_exe_lift(gen, files):
    """An exe lift's out/gen: recomp_NNNN.c defining the bodies of files[N], their declarations
    and a dispatch table of all of them."""
    gen.mkdir(parents=True, exist_ok=True)
    every = sorted(va for f in files for va in f)
    for i, vas in enumerate(files):
        (gen / f'recomp_{i:04d}.c').write_text(
            '#include "recomp_types.h"\n\n'
            + ''.join(f'void sub_{va:08X}(void) {{\n    RECOMP_ENTER(0x{va:08X}u);\n}}\n\n'
                      for va in vas))
    (gen / 'recomp_funcs.h').write_text(''.join(f'void sub_{va:08X}(void);\n' for va in every))
    (gen / 'recomp_dispatch.c').write_text(
        'const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n'
        + ''.join(f'    {{ 0x{va:08X}u, sub_{va:08X} }},\n' for va in every)
        + '};\nconst uint32_t sfcop_entry_va = 0x00400000u;\n')
    return gen


def ret_object(*functions):
    """One COMDAT section per (symbol, bytes), each a function."""
    o = mc.Obj()
    for sym, code in functions:
        t = o.section('.text', code, mc.CODE | mc.COMDAT | mc.ALIGN_16, selection=mc.NODUPLICATES)
        o.define(sym, t, 0, function=True)
    return o


XOR_RET = b'\x33\xc0\xc3'   # xor eax, eax / ret


def two_unit_inputs(tmp_path, claims):
    """Units Test/A.cpp (_a) and Test/B.cpp (_b, _b2); claims: [(address, symbol, unit)]."""
    write(tmp_path, 'A.obj', ret_object(('_a', XOR_RET)))
    write(tmp_path, 'B.obj', ret_object(('_b', XOR_RET), ('_b2', XOR_RET)))
    with open(tmp_path / 'manifest.tsv', 'w', newline='') as f:
        f.write('address\tsize\tsymbol\tsource\tmodes\n')
        for address, sym, unit in claims:
            f.write(f'0x{address:08x}\t3\t{sym[1:]}()\tTest/{unit}.cpp\tvc6\n')
    with open(tmp_path / 'functions.csv', 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(FUNCTIONS_HEADER)
        for address, sym, _unit in claims:
            w.writerow([f'{address:08x}', 3, sym[1:], sym, '', 'test', '', '', '', '0', '',
                        'function', ''])
    (tmp_path / 'sdk.tsv').write_text('address\tname\n')
    (tmp_path / 'game').mkdir(exist_ok=True)


def emit_by_hand(tmp_path, out, gen):
    """objlift's placement and emit, each claimed body lifted 'by hand' to a one-line C body that
    calls the function at the next claim's address, as a lifted call would."""
    import argparse
    import liftcode
    a = argparse.Namespace(
        symbols=str(tmp_path / 'functions.csv'), sdk_symbols=[str(tmp_path / 'sdk.tsv')], exe=None,
        obj=[f'Test/A.cpp={tmp_path / "A.obj"}', f'Test/B.cpp={tmp_path / "B.obj"}'],
        lifted=str(gen), manifest=str(tmp_path / 'manifest.tsv'), game=str(tmp_path / 'game'))
    run = objlift.Run(a)
    claims, result = run.place()
    bodies = []
    for c in claims:
        sec = next(s for s in result.sections if c in s.claims)
        bodies.append(liftcode.Body(
            c.address, 'sub_%08X' % c.address,
            'void sub_%08X(void) { sfcop_guest_entered(%s); }\n' % (c.address,
                                                                  objlift.index_name(c.address)),
            sec, True))
    files = objlift.emit(run, claims, result, bodies)
    return objlift.write_or_check(str(out), files, False)


def unit_file(unit, decls, bodies):
    """A unit's code file as it is written: the head, then decls and bodies as given."""
    return ('/* Generated by tools/objlift/objlift.py from the VC6 objects of the claimed units. */\n'
            f'/* game/Test/{unit}.cpp */\n'
            '#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n#include "recomp_funcs.h"\n'
            '#include "app/guest/override.h"\n#include <math.h>\n#include <string.h>\n\n'
            + decls + '\n' + bodies)


A_FILE = unit_file('A', 'void sub_00402000(void);\n'
                   'extern const unsigned sfcop_objlift_index_00402000;\n',
                   '/* 0x00402000 claimed .text, game/Test/A.cpp */\n'
                   'void sub_00402000(void) { sfcop_guest_entered(sfcop_objlift_index_00402000); }\n\n')
B_FILE = unit_file('B', 'void sub_00403000(void);\n'
                   'extern const unsigned sfcop_objlift_index_00403000;\n',
                   '/* 0x00403000 claimed .text, game/Test/B.cpp */\n'
                   'void sub_00403000(void) { sfcop_guest_entered(sfcop_objlift_index_00403000); }\n\n')


def test_the_code_file_name_is_the_units_path_as_a_c_identifier():
    assert objlift.code_file_name('Common/DynamicAllocator.cpp') == \
        'objlift_code_Common_DynamicAllocator.c'
    assert objlift.code_file_name('unsorted/t3DListenerProfile.cpp') == \
        'objlift_code_unsorted_t3DListenerProfile.c'
    assert objlift.code_file_name('3d/a-b.c') == 'objlift_code__3d_a_b.c'


def test_read_lift_files_says_which_exe_lift_file_defines_each_body(tmp_path):
    gen = fake_exe_lift(tmp_path, [[0x00401000], [0x00402000, 0x00403000]])
    assert objlift.read_lift_files(str(gen)) == (
        {0x00401000: '0000', 0x00402000: '0001', 0x00403000: '0001'}, ['0000', '0001'])
    (gen / 'recomp_0002.c').write_text('void sub_00401000(void) {\n}\n')
    with pytest.raises(PlacementError, match='sub_00401000 is defined in both recomp_0000.c and '
                                             'recomp_0002.c'):
        objlift.read_lift_files(str(gen))


def test_each_unit_and_each_exe_lift_file_gets_its_own_file(tmp_path):
    two_unit_inputs(tmp_path, [(0x00402000, '_a', 'A'), (0x00403000, '_b', 'B')])
    gen = fake_exe_lift(tmp_path / 'gen', [[0x00401000], [0x00402000], [0x00403000], []])
    out = tmp_path / 'out'
    emit_by_hand(tmp_path, out, gen)
    assert sorted(os.listdir(out)) == [
        'objlift_code_Test_A.c', 'objlift_code_Test_B.c', 'objlift_dispatch.c', 'objlift_funcs.h',
        'objlift_report.txt', 'objlift_table.c', 'objlift_weak_0000.h', 'objlift_weak_0001.h',
        'objlift_weak_0002.h', 'objlift_weak_0003.h']
    assert (out / 'objlift_code_Test_A.c').read_text() == A_FILE
    assert (out / 'objlift_code_Test_B.c').read_text() == B_FILE
    assert (out / 'objlift_weak_0000.h').read_text() == WEAK_HEAD % '0000'
    assert (out / 'objlift_weak_0001.h').read_text() == (WEAK_HEAD % '0001'
                                                         + '#pragma weak sub_00402000\n')
    assert (out / 'objlift_weak_0002.h').read_text() == (WEAK_HEAD % '0002'
                                                         + '#pragma weak sub_00403000\n')
    assert (out / 'objlift_weak_0003.h').read_text() == WEAK_HEAD % '0003'
    table = (out / 'objlift_table.c').read_text()
    assert ('const unsigned sfcop_objlift_index_00402000 = 0u;\n'
            'const unsigned sfcop_objlift_index_00403000 = 1u;\n') in table


def test_a_second_emit_of_the_same_input_touches_nothing(tmp_path):
    two_unit_inputs(tmp_path, [(0x00402000, '_a', 'A'), (0x00403000, '_b', 'B')])
    gen = fake_exe_lift(tmp_path / 'gen', [[0x00401000], [0x00402000], [0x00403000], []])
    out = tmp_path / 'out'
    assert len(emit_by_hand(tmp_path, out, gen)) == 10
    for n in os.listdir(out):
        os.utime(out / n, ns=(1_000_000_000, 1_000_000_000))
    assert emit_by_hand(tmp_path, out, gen) == []
    assert {os.stat(out / n).st_mtime_ns for n in os.listdir(out)} == {1_000_000_000}


def test_a_new_claim_rewrites_only_its_units_file_and_its_weak_header(tmp_path):
    two_unit_inputs(tmp_path, [(0x00402000, '_a', 'A'), (0x00403000, '_b', 'B')])
    gen = fake_exe_lift(tmp_path / 'gen', [[0x00401000], [0x00402000], [0x00403000], []])
    out = tmp_path / 'out'
    emit_by_hand(tmp_path, out, gen)
    for n in os.listdir(out):
        os.utime(out / n, ns=(1_000_000_000, 1_000_000_000))
    # B's second function, claimed below A's: every index in the table moves up by one
    two_unit_inputs(tmp_path, [(0x00401000, '_b2', 'B'), (0x00402000, '_a', 'A'),
                               (0x00403000, '_b', 'B')])
    changed = emit_by_hand(tmp_path, out, gen)
    assert changed == ['objlift_code_Test_B.c', 'objlift_funcs.h', 'objlift_report.txt',
                       'objlift_table.c', 'objlift_weak_0000.h']
    # the dispatch table changes only with a fresh body: the exe lift's already names 0x00401000
    untouched = ['objlift_code_Test_A.c', 'objlift_dispatch.c', 'objlift_weak_0001.h',
                 'objlift_weak_0002.h', 'objlift_weak_0003.h']
    assert {os.stat(out / n).st_mtime_ns for n in untouched} == {1_000_000_000}
    assert (out / 'objlift_code_Test_A.c').read_text() == A_FILE
    assert (out / 'objlift_weak_0000.h').read_text() == (WEAK_HEAD % '0000'
                                                         + '#pragma weak sub_00401000\n')
    table = (out / 'objlift_table.c').read_text()
    assert ('const unsigned sfcop_objlift_index_00401000 = 0u;\n'
            'const unsigned sfcop_objlift_index_00402000 = 1u;\n'
            'const unsigned sfcop_objlift_index_00403000 = 2u;\n') in table


def test_a_unit_no_longer_claimed_loses_its_file_and_the_old_single_files_go(tmp_path):
    two_unit_inputs(tmp_path, [(0x00402000, '_a', 'A'), (0x00403000, '_b', 'B')])
    gen = fake_exe_lift(tmp_path / 'gen', [[0x00401000], [0x00402000], [0x00403000], []])
    out = tmp_path / 'out'
    out.mkdir()
    (out / 'objlift_code.c').write_text('old\n')
    (out / 'objlift_weak.h').write_text('old\n')
    (out / 'objlift_unrelated.txt').write_text('kept\n')
    emit_by_hand(tmp_path, out, gen)
    assert not (out / 'objlift_code.c').exists() and not (out / 'objlift_weak.h').exists()
    two_unit_inputs(tmp_path, [(0x00402000, '_a', 'A')])
    emit_by_hand(tmp_path, out, gen)
    assert not (out / 'objlift_code_Test_B.c').exists()
    assert (out / 'objlift_code_Test_A.c').read_text() == A_FILE
    assert (out / 'objlift_weak_0002.h').read_text() == WEAK_HEAD % '0002'
    assert (out / 'objlift_unrelated.txt').read_text() == 'kept\n'


# --- against the install ---------------------------------------------------------------------------

@pytest.mark.retail
def test_the_real_dynamic_allocator_object_lifts_onto_the_exe(tmp_path):
    if not os.path.isfile(paths.EXE):
        skip('the original exe is not in the install folder')
    if shutil.which('docker') is None:
        skip('docker is not installed, and the pinned VC6 runs in its container')
    rows = []
    with open(os.path.join(REPO, 'game', 'manifest.tsv'), newline='') as f:
        for r in csv.DictReader(f, delimiter='\t'):
            if r['source'] == 'Common/DynamicAllocator.cpp' and 'vc6' in r['modes'].split():
                rows.append(r)
    assert [r['address'] for r in rows] == ['0x006fc4d0', '0x006fc5a0', '0x006fc750']
    manifest = tmp_path / 'manifest.tsv'
    with open(manifest, 'w', newline='') as f:
        w = csv.DictWriter(f, rows[0].keys(), delimiter='\t', lineterminator='\n')
        w.writeheader()
        w.writerows(rows)
    import compile as objcompile
    built = tmp_path / 'obj'
    assert objcompile.main(['--manifest', str(manifest), '--out', str(built)]) == 0
    args = ['--obj', f'Common/DynamicAllocator.cpp={built / "Common" / "DynamicAllocator.obj"}',
            '--manifest', str(manifest), '--symbols', os.path.join(REPO, 'ghidra', 'exports'),
            '--exe', paths.EXE, '--out', str(tmp_path / 'out'), '--no-lift']
    assert objlift.main(args) == 0
    text = (tmp_path / 'out' / 'objlift_report.txt').read_text()
    for address in ('0x006fc4d0', '0x006fc5a0', '0x006fc750'):
        assert any(line.startswith(f'  {address} ') and line.endswith('identical to the exe')
                   for line in text.splitlines()), address
    relocs = text.split('\nrelocations:\n')[1].split('\n\n')[0].splitlines()
    assert {line.split()[1] for line in relocs} == {'REL32', 'DIR32'}
    assert 'data image: empty\n' in text


def test_partial_and_stub_rows_are_neither_compiled_nor_lifted(tmp_path):
    # tools/manifest.py gives a FUNCTION not recorded exact the mode `partial`: the native build
    # keeps the exe lift's body for it, so neither the compile step nor the placement reads it.
    m = tmp_path / 'manifest.tsv'
    m.write_text('address\tsize\tsymbol\tsource\tmodes\n'
                 '0x00401000\t24\tA()\tTest/Exact.cpp\tvc6 guest\n'
                 '0x00401100\t16\tB()\tTest/Exact.cpp\tpartial\n'
                 '0x00401200\t40\tC()\tTest/Waiting.cpp\tpartial\n'
                 '0x00401300\t8\tD()\tTest/Stubbed.cpp\tstub\n'
                 '0x00401400\t20\tE::E()\tTest/Ctor.cpp\tvc6\n')
    assert objlift.read_manifest(str(m)) == [(0x00401000, 24, 'A()', 'Test/Exact.cpp'),
                                             (0x00401400, 20, 'E::E()', 'Test/Ctor.cpp')]
    assert objcompile.claimed_units(str(m)) == ['Test/Exact.cpp', 'Test/Ctor.cpp']


def test_the_compile_key_follows_the_unit_and_every_header(tmp_path):
    game = tmp_path / 'game'
    (game / 'Test').mkdir(parents=True)
    (game / 'Test' / 'A.cpp').write_text('#include "A.h"\nint a;\n')
    (game / 'Test' / 'B.cpp').write_text('int b;\n')
    (game / 'Test' / 'A.h').write_text('int x;\n')
    (game / 'manifest.tsv').write_text('address\n')
    profile = {'name': 'p', 'flags': '/O1'}

    def keys():
        shared = objcompile.shared_key(str(game))
        return [objcompile.unit_key(shared, f'Test/{u}.cpp', str(game / 'Test' / f'{u}.cpp'),
                                    profile) for u in 'AB']
    a, b = keys()
    assert keys() == [a, b]
    (game / 'Test' / 'A.cpp').write_text('#include "A.h"\nint a;  // a comment\n')
    a2, b2 = keys()
    assert a2 != a and b2 == b                       # one unit's source: that unit only
    (game / 'manifest.tsv').write_text('address\nmore\n')
    assert keys() == [a2, b2]                        # the manifest is not a compile input
    (game / 'Test' / 'A.h').write_text('int y;\n')
    a3, b3 = keys()
    assert a3 != a2 and b3 != b2                     # any header: every unit
    assert objcompile.unit_key(objcompile.shared_key(str(game)), 'Test/B.cpp',
                               str(game / 'Test' / 'B.cpp'), dict(profile, flags='/O2')) != b3


# A unit with tShip's out-of-class destructor and the /FAs listing VC6 writes for it: the
# destructor has a line record (3), the scalar deleting destructor generated from it has none.
DELETING_UNIT = '// FUNCTION: STARFLEETOP 0x00401500\ntShip::~tShip()\n{\n}\n'
DELETING_LISTING = (
    '\tTITLE\tship.cpp\n'
    '_TEXT\tSEGMENT\n'
    '??1tShip@@UAE@XZ PROC NEAR\t\t; tShip::~tShip, COMDAT\n\n'
    '; 3    : {\n\n'
    '\tret\t0\n'
    '??1tShip@@UAE@XZ ENDP\n'
    '_TEXT\tENDS\n'
    '_TEXT\tSEGMENT\n'
    '??_GtShip@@UAEPAXI@Z PROC NEAR\t\t; tShip::`scalar deleting destructor\', COMDAT\n'
    '\tcall\t??1tShip@@UAE@XZ\t\t; tShip::~tShip\n'
    '\tret\t4\n'
    '??_GtShip@@UAEPAXI@Z ENDP\n'
    '_TEXT\tENDS\n'
    'END\n')


def test_the_lift_claims_a_deleting_destructor_the_matched_set_alone_names(tmp_path):
    # functions.csv has no mangled name and a placeholder name for 0x00401500; only its
    # matched-set entry says it is tShip's scalar deleting destructor. The lift places ??_GtShip
    # there, as the harness scores it and tools/manifest.py names the row, not the destructor.
    game = tmp_path / 'game'
    (game / 'Test').mkdir(parents=True)
    (game / 'Test' / 'ship.cpp').write_text(DELETING_UNIT)
    (tmp_path / 'ship.asm').write_text(DELETING_LISTING)
    obj = str(tmp_path / 'ship.obj')
    recorded = {'symbol': '??_GtShip@@UAEPAXI@Z', 'name': 'FUN_00401500', 'exact': True}
    assert objlift.claimed_symbol(0x00401500, 'Test/ship.cpp', obj, str(game), '', 'FUN_00401500',
                                  recorded) == '??_GtShip@@UAEPAXI@Z'
    assert objlift.claimed_symbol(0x00401500, 'Test/ship.cpp', obj, str(game), '',
                                  'FUN_00401500') == '??1tShip@@UAE@XZ'


# tShip's destructor written inline in its header, with the claim of its scalar deleting
# destructor on it (line 3), and the listing of the unit the unit map assigns 0x00401500 to: VC6
# inlined the destructor, so there is no ??1tShip to start from.
INLINE_DELETING_HEADER = ('class tShip { public: virtual ~tShip(); void mClear(); };\n'
                          '\n'
                          '// FUNCTION: STARFLEETOP 0x00401500\n'
                          'inline tShip::~tShip()\n{\n\tmClear();\n}\n')
INLINED_LISTING = (
    '\tTITLE\tship.cpp\n'
    '_TEXT\tSEGMENT\n'
    '??_GtShip@@UAEPAXI@Z PROC NEAR\t\t; tShip::`scalar deleting destructor\', COMDAT\n'
    '\tcall\t?mClear@tShip@@QAEXXZ\t\t; tShip::mClear\n'
    '\tret\t4\n'
    '??_GtShip@@UAEPAXI@Z ENDP\n'
    '_TEXT\tENDS\n'
    'END\n')


def _header_tree(tmp_path, units):
    game = tmp_path / 'game'
    (game / 'Test').mkdir(parents=True)
    (game / 'Include').mkdir()
    (game / 'Include' / 'ship.h').write_text(INLINE_DELETING_HEADER)
    (game / 'Test' / 'ship.cpp').write_text('#include "Include/ship.h"\n')
    (game / 'Test' / 'other.cpp').write_text('#include "Include/ship.h"\n')
    (game / 'UNITS.tsv').write_text('unit\tfirst\tlast\n' + units)
    (tmp_path / 'ship.asm').write_text(INLINED_LISTING)
    return game


def test_the_lift_claims_a_deleting_destructor_claimed_in_a_header(tmp_path):
    game = _header_tree(tmp_path, 'Test/ship.cpp\t0x00401000\t0x004016ff\n')
    recorded = {'symbol': '??_GtShip@@UAEPAXI@Z', 'name': 'FUN_00401500', 'exact': True}
    assert objlift.claimed_symbol(0x00401500, 'Test/ship.cpp', str(tmp_path / 'ship.obj'),
                                  str(game), '', 'FUN_00401500', recorded) == '??_GtShip@@UAEPAXI@Z'


def test_the_lift_refuses_a_header_claim_the_unit_map_places_in_another_unit(tmp_path):
    game = _header_tree(tmp_path, 'Test/other.cpp\t0x00401000\t0x004016ff\n')
    recorded = {'symbol': '??_GtShip@@UAEPAXI@Z', 'name': 'FUN_00401500', 'exact': True}
    with pytest.raises(objlift.PlacementError, match='its claim in Include/ship.h is compiled in '
                       'Test/other.cpp, not in Test/ship.cpp'):
        objlift.claimed_symbol(0x00401500, 'Test/ship.cpp', str(tmp_path / 'ship.obj'), str(game),
                               '', 'FUN_00401500', recorded)
