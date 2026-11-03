#!/usr/bin/env python3
"""Static census of StarFleetOP.exe for the lifter spike: exception handling,
fs: accesses, DirectX interface GUIDs and COM vtable-slot calls.

    python3 census.py <exe> <functions.json> <out_dir>

Writes <out_dir>/eh_census.json, guids.tsv, com_calls.tsv and prints a summary.
Everything here is pattern matching over the original bytes (pefile +
Capstone); it does not depend on the lifter. The COM call detector:

    mov  rV, [rO]          ; rO holds an object, rV its vtable
    ...  push rO           ; last push before the call is the object (stdcall this)
    call [rV + 4*slot]

Plain C++ virtual calls pass `this` in ecx and are not counted unless they
also push the object. A call site is reported with the operand rO was loaded
from (global address or [reg+field]) so call sites can be grouped by the
object slot they use.
"""
import bisect
import collections
import json
import os
import struct
import sys
import uuid

import pefile
from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

EH_MAGIC_VC6 = 0x19930520
CXX_EXC_CODE = 0xE06D7363

GUIDS = {
    'IID_IDirectDraw': '6C14DB80-A733-11CE-A521-0020AF0BE560',
    'IID_IDirectDraw2': 'B3A6F3E0-2B43-11CF-A2DE-00AA00B93356',
    'IID_IDirectDraw4': '9C59509A-39BD-11D1-8C4A-00C04FD930C5',
    'IID_IDirectDraw7': '15E65EC0-3B9C-11D2-B92F-00609797EA5B',
    'IID_IDirectDrawSurface': '6C14DB81-A733-11CE-A521-0020AF0BE560',
    'IID_IDirectDrawSurface2': '57805885-6EEC-11CF-9441-A82303C10E27',
    'IID_IDirectDrawSurface3': 'DA044E00-69B2-11D0-A1D5-00AA00B8DFBB',
    'IID_IDirectDrawSurface4': '0B2B8630-AD35-11D0-8EA6-00609797EA5B',
    'IID_IDirectDrawSurface7': '06675A80-3B9B-11D2-B92F-00609797EA5B',
    'IID_IDirectDrawPalette': '6C14DB84-A733-11CE-A521-0020AF0BE560',
    'IID_IDirectDrawClipper': '6C14DB85-A733-11CE-A521-0020AF0BE560',
    'IID_IDirectDrawGammaControl': '69C11C3E-B46B-11D1-AD7A-00C04FC29B4E',
    'IID_IDirect3D': '3BBA0080-2421-11CF-A31A-00AA00B93356',
    'IID_IDirect3D2': '6AAE1EC1-662A-11D0-889D-00AA00BBB76A',
    'IID_IDirect3D3': 'BB223240-E72B-11D0-A9B4-00AA00C0993E',
    'IID_IDirect3D7': 'F5049E77-4861-11D2-A407-00A0C90629A8',
    'IID_IDirect3DHALDevice': '84E63DE0-46AA-11CF-816F-0000C020156E',
    'IID_IDirect3DTnLHalDevice': 'F5049E78-4861-11D2-A407-00A0C90629A8',
    'IID_IDirect3DRGBDevice': 'A4665C60-2673-11CF-A31A-00AA00B93356',
    'IID_IDirect3DRefDevice': '50936643-13E9-11D1-89AA-00A0C9054129',
    'IID_IDirect3DMMXDevice': '881949A1-D6F3-11D0-89AB-00A0C9054129',
    'IID_IDirect3DNullDevice': '8767DF22-BACC-11D1-8969-00A0C90629A8',
    'IID_IDirectSound': '279AFA83-4981-11CE-A521-0020AF0BE560',
    'IID_IDirectSoundBuffer': '279AFA85-4981-11CE-A521-0020AF0BE560',
    'IID_IDirectSound3DListener': '279AFA84-4981-11CE-A521-0020AF0BE560',
    'IID_IDirectSound3DBuffer': '279AFA86-4981-11CE-A521-0020AF0BE560',
    'IID_IDirectInputA': '89521360-AA8A-11CF-BFC7-444553540000',
    'IID_IDirectInput2A': '5944E662-AA8A-11CF-BFC7-444553540000',
    'IID_IDirectInput7A': '9A4CB684-236D-11D3-8E9D-00C04F6844AE',
    'IID_IDirectInputDeviceA': '5944E680-C92E-11CF-BFC7-444553540000',
    'IID_IDirectInputDevice2A': '5944E682-C92E-11CF-BFC7-444553540000',
    'IID_IDirectInputDevice7A': '57D7C6BC-2356-11D3-8E9D-00C04F6844AE',
    'GUID_SysMouse': '6F1D2B60-D5A0-11CF-BFC7-444553540000',
    'GUID_SysKeyboard': '6F1D2B61-D5A0-11CF-BFC7-444553540000',
    'GUID_Joystick': '6F1D2B70-D5A0-11CF-BFC7-444553540000',
    'IID_IDirectPlay4A': '0AB1C531-4745-11D1-A7A1-0000F803ABFC',
    'IID_IDirectPlayLobby3A': '2DB72491-652C-11D1-A7A8-0000F803ABFC',
    'CLSID_DirectPlay': 'D1EB6D20-8923-11D0-9D97-00A0C90A43CB',
    'CLSID_DirectPlayLobby': '2FE8F810-B2A5-11D0-A787-0000F803ABFC',
}


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.secs = []
        for s in self.pe.sections:
            name = s.Name.rstrip(b'\0').decode()
            data = s.get_data()
            va = self.base + s.VirtualAddress
            size = max(s.Misc_VirtualSize, len(data))
            self.secs.append((name, va, size, data))
        self.text = next(x for x in self.secs if x[0] == '.text')

    def read(self, va, n):
        for _, sva, size, data in self.secs:
            if sva <= va and va + n <= sva + size:
                off = va - sva
                b = data[off:off + n]
                return b + b'\0' * (n - len(b))
        return None

    def u32(self, va):
        b = self.read(va, 4)
        return struct.unpack('<I', b)[0] if b else None

    def section_of(self, va):
        for name, sva, size, _ in self.secs:
            if sva <= va < sva + size:
                return name
        return None


def find_all(data, needle):
    i = data.find(needle)
    while i != -1:
        yield i
        i = data.find(needle, i + 1)


def eh_census(img, md):
    _, tva, _, tdata = img.text
    out = collections.OrderedDict()
    # C++ EH: handler stubs `mov eax, FuncInfo; jmp __CxxFrameHandler`.
    stubs, fis, handler_targets = [], {}, collections.Counter()
    for i in find_all(tdata, b'\xB8'):
        if i + 10 > len(tdata) or tdata[i + 5] != 0xE9:
            continue
        fi = struct.unpack_from('<I', tdata, i + 1)[0]
        if img.u32(fi) != EH_MAGIC_VC6:
            continue
        tgt = (tva + i + 10 + struct.unpack_from('<i', tdata, i + 6)[0]) & 0xFFFFFFFF
        stubs.append(tva + i)
        handler_targets[tgt] += 1
        fis[fi] = tva + i
    n_unwind = n_try = n_catch = 0
    catch_types = collections.Counter()
    funclets = set()
    for fi in fis:
        max_state, p_unwind, n_t, p_try = struct.unpack('<4I', img.read(fi + 4, 16))
        n_unwind += max_state
        n_try += n_t
        for t in range(min(n_t, 4096)):
            row = img.read(p_try + 20 * t, 20)
            if not row:
                break
            _lo, _hi, _chi, n_c, p_h = struct.unpack('<5I', row)
            for c in range(min(n_c, 256)):
                h = img.read(p_h + 16 * c, 16)
                if not h:
                    break
                adj, p_type, _disp, addr = struct.unpack('<4I', h)
                n_catch += 1
                funclets.add(addr)
                if p_type:
                    nm = img.read(p_type + 8, 64) or b''
                    catch_types[nm.split(b'\0')[0].decode('latin-1')] += 1
                else:
                    catch_types['...'] += 1
    out['cxx_handler_stubs'] = len(stubs)
    out['cxx_funcinfo_tables'] = len(fis)
    out['cxx_frame_handler_targets'] = {'0x%08X' % k: v for k, v in handler_targets.most_common()}
    out['cxx_unwind_states_total'] = n_unwind
    out['cxx_try_blocks'] = n_try
    out['cxx_catch_handlers'] = n_catch
    out['cxx_catch_funclets_unique'] = len(funclets)
    out['cxx_catch_types'] = dict(catch_types.most_common(40))
    # fs: accesses by displacement, and the two halves of a frame install.
    # Byte scan for the fs: prefix (0x64), decoding one instruction at each hit.
    # Only absolute TIB offsets below 0x1000 are kept: a 0x64 byte inside some
    # other instruction decodes to fs:[reg+...] or a large displacement, so the
    # filter drops that noise (a few false hits may remain).
    fs = collections.Counter()
    for i in find_all(tdata, b'\x64'):
        chunk = tdata[i:i + 16]
        for ins in md.disasm(chunk, tva + i, count=1):
            if 'fs:[' in ins.op_str:
                disp = ins.op_str.split('fs:[', 1)[1].split(']', 1)[0]
                try:
                    d = int(disp, 0)
                except ValueError:
                    continue
                if d < 0x1000:
                    fs['%s %s' % (ins.mnemonic, ins.op_str)] += 1
    out['fs_accesses'] = dict(fs.most_common())
    # SEH (_except_handler3): `push -1; push scopetable; push handler; mov eax, fs:[0]`.
    seh = collections.Counter()
    for i in find_all(tdata, b'\x6A\xFF\x68'):
        if tdata[i + 7:i + 8] == b'\x68' and tdata[i + 12:i + 14] == b'\x64\xA1':
            seh[struct.unpack_from('<I', tdata, i + 8)[0]] += 1
    out['seh_frames_by_handler'] = {'0x%08X' % k: v for k, v in seh.most_common()}
    # Throws: _CxxThrowException is the function that pushes 0xE06D7363.
    sites = list(find_all(tdata, b'\x68' + struct.pack('<I', CXX_EXC_CODE)))
    out['push_E06D7363_sites'] = ['0x%08X' % (tva + i) for i in sites]
    return out, stubs


def call_sites(img, target):
    _, tva, _, tdata = img.text
    n = 0
    for i in find_all(tdata, b'\xE8'):
        if i + 5 <= len(tdata) and (tva + i + 5 + struct.unpack_from('<i', tdata, i + 1)[0]) & 0xFFFFFFFF == target:
            n += 1
    return n


def guid_scan(img):
    rows = []
    for name, g in GUIDS.items():
        raw = uuid.UUID(g).bytes_le
        for sname, sva, _, data in img.secs:
            for i in find_all(data, raw):
                rows.append((name, '0x%08X' % (sva + i), sname))
    return rows


def mem_key(op, ins):
    m = op.mem
    base = ins.reg_name(m.base) if m.base else ''
    idx = ins.reg_name(m.index) if m.index else ''
    if not base and not idx:
        return 'g_%08X' % (m.disp & 0xFFFFFFFF)
    s = '[%s%s%+#x]' % (base, ('+%s*%d' % (idx, m.scale)) if idx else '', m.disp)
    return s


def com_scan(img, md, funcs):
    """Yield (func, site, slot, source) for stdcall-style COM calls."""
    md.detail = True
    _, tva, tsize, tdata = img.text
    for f in funcs:
        a, end = f['address'], min(f.get('end', f['address'] + 0x400), f['address'] + 0x4000)
        if not (tva <= a < tva + tsize):
            continue
        obj_src = {}     # reg -> source key of the object it holds
        vtbl_of = {}     # reg -> reg (object) whose vtable it holds
        last_push = None
        for ins in md.disasm(tdata[a - tva:end - tva], a):
            mn, ops = ins.mnemonic, ins.operands
            if mn == 'mov' and len(ops) == 2 and ops[0].type == X86_OP_REG:
                dst = ins.reg_name(ops[0].reg)
                obj_src.pop(dst, None)
                vtbl_of.pop(dst, None)
                if ops[1].type == X86_OP_MEM:
                    m = ops[1].mem
                    if m.base and not m.index and m.disp == 0 and ins.reg_name(m.base) in obj_src \
                            and ops[1].size == 4:
                        vtbl_of[dst] = ins.reg_name(m.base)
                    elif ops[1].size == 4:
                        obj_src[dst] = mem_key(ops[1], ins)
                elif ops[1].type == X86_OP_REG and ins.reg_name(ops[1].reg) in obj_src:
                    obj_src[dst] = obj_src[ins.reg_name(ops[1].reg)]
            elif mn == 'push':
                if ops and ops[0].type == X86_OP_REG:
                    last_push = ins.reg_name(ops[0].reg)
                elif ops and ops[0].type == X86_OP_MEM:
                    last_push = ('mem', mem_key(ops[0], ins))
                else:
                    last_push = None
            elif mn == 'call':
                if ops and ops[0].type == X86_OP_MEM and ops[0].mem.base and not ops[0].mem.index:
                    vreg = ins.reg_name(ops[0].mem.base)
                    disp = ops[0].mem.disp
                    if vreg in vtbl_of and disp % 4 == 0 and 0 <= disp < 0x200:
                        oreg = vtbl_of[vreg]
                        pushed = last_push == oreg or (isinstance(last_push, tuple)
                                                       and last_push[1] == obj_src.get(oreg))
                        if pushed:
                            yield a, ins.address, disp // 4, obj_src.get(oreg, '?')
                obj_src.clear()
                vtbl_of.clear()
                last_push = None
            elif mn in ('ret', 'jmp'):
                obj_src.clear()
                vtbl_of.clear()
                last_push = None
            else:
                # any other write to a tracked register kills it
                if ops and ops[0].type == X86_OP_REG and mn not in ('cmp', 'test', 'push'):
                    r = ins.reg_name(ops[0].reg)
                    obj_src.pop(r, None)
                    vtbl_of.pop(r, None)


def main():
    exe, catalog, out_dir = sys.argv[1:4]
    os.makedirs(out_dir, exist_ok=True)
    img = Image(exe)
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    eh, _ = eh_census(img, md)
    # The throw path. VC6's static _CxxThrowException keeps 0xE06D7363 in a
    # static template, so it is found as a caller of RaiseException through the
    # IAT; each caller's entry is the nearest catalogued start at or below the
    # call, and its direct call sites are counted.
    funcs = [f for f in json.load(open(catalog))['functions'] if f.get('entry_kind') != 'alias']
    starts = sorted(f['address'] for f in funcs)
    img.pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
    iat = {}
    for d in img.pe.DIRECTORY_ENTRY_IMPORT:
        for imp in d.imports:
            if imp.name:
                iat[imp.name.decode()] = imp.address
    _, tva, _, tdata = img.text
    raisers = collections.Counter()
    for i in find_all(tdata, b'\xFF\x15' + struct.pack('<I', iat['RaiseException'])):
        k = bisect.bisect_right(starts, tva + i) - 1
        raisers[starts[k]] += 1
    eh['raiseexception_callers'] = {'0x%08X' % a: {'raise_sites': n, 'direct_calls_to_it': call_sites(img, a)}
                                    for a, n in raisers.items()}
    tmpl = list(find_all(b''.join(x[3] for x in img.secs[1:]), struct.pack('<I', CXX_EXC_CODE)))
    eh['e06d7363_dwords_outside_text'] = len(tmpl)
    json.dump(eh, open(os.path.join(out_dir, 'eh_census.json'), 'w'), indent=1)

    rows = guid_scan(img)
    with open(os.path.join(out_dir, 'guids.tsv'), 'w') as f:
        f.write('guid\tva\tsection\n')
        for r in rows:
            f.write('\t'.join(r) + '\n')

    calls = list(com_scan(img, md, funcs))
    with open(os.path.join(out_dir, 'com_calls.tsv'), 'w') as f:
        f.write('function\tsite\tslot\tobject_source\n')
        for fa, site, slot, src in sorted(set(calls)):
            f.write('0x%08X\t0x%08X\t%d\t%s\n' % (fa, site, slot, src))

    print('C++ EH: %d FuncInfo tables, %d try blocks, %d catch handlers (%d unique funclets), %d unwind states'
          % (eh['cxx_funcinfo_tables'], eh['cxx_try_blocks'], eh['cxx_catch_handlers'],
             eh['cxx_catch_funclets_unique'], eh['cxx_unwind_states_total']))
    print('  __CxxFrameHandler targets: %s' % eh['cxx_frame_handler_targets'])
    print('SEH frames by handler: %s' % eh['seh_frames_by_handler'])
    print('RaiseException callers: %s' % eh['raiseexception_callers'])
    print('fs: accesses: %s' % eh['fs_accesses'])
    print('GUIDs found: %s' % sorted({r[0] for r in rows}))
    print('COM-style call sites: %d in %d functions' % (len(set(calls)), len({c[0] for c in calls})))


if __name__ == '__main__':
    main()
