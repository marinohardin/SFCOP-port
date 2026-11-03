#!/usr/bin/env python3
"""DirectX method inventory: which vtable slots the game calls, per interface,
and whether pcrecomp's win32hle answers each one.

    python3 dx_inventory.py <results/com_calls.tsv> <pcrecomp/runtime/win32hle> <out.md>

Input is census.py's com_calls.tsv (stdcall COM-style call sites with the
object field they load the interface from). The interface of a field is
assigned below from evidence, not from type recovery:

* the code ranges are where the IID constants are referenced (guids.tsv xrefs:
  IDirectDraw4/IDirect3D3 in 0x0069D3F0..0x006B79F0, IDirectSound3D* at
  0x006434E0/0x00645BE0, GUID_SysKeyboard at 0x004013F0);
* within a range, a field is named by the set of slots called through it, when
  that set is only coherent for one interface (e.g. +0x7F64 is called with
  9/10/22/28/38/40/41 = BeginScene/EndScene/SetRenderState/DrawPrimitive/
  SetTexture/SetTextureStageState/ValidateDevice, which only IDirect3DDevice3
  has in those slots). Fields whose slot set fits several interfaces stay
  unattributed and are listed as such.

Slot names come from win32hle's own vtable tables (d3d.c, ddraw.c, dsound.c),
which list "Interface::Method" in slot order; for interfaces win32hle lacks
(DirectSound3D, DirectInput) the names are transcribed from the DirectX 6 SDK
headers' method order and marked as such.
"""
import collections
import csv
import os
import re
import sys

RANGES = {
    'dmp3d': (0x0069D000, 0x006B9000),
    'sound': (0x00640000, 0x00647000),
    'dinput': (0x00401000, 0x00402000),
}
# (range, field) -> interface
FIELDS = {
    ('dmp3d', '+0x7f64'): 'IDirect3DDevice3',
    ('dmp3d', '+0x58'): 'IDirectDrawSurface4',
    ('dmp3d', '+0x30'): 'IDirectDrawSurface4',
    ('dmp3d', '+0x34'): 'IDirectDrawSurface4',
    ('dmp3d', '+0x3c'): 'IDirectDrawSurface4',
    ('dmp3d', '+0x618'): 'IDirectDraw4',
    ('dmp3d', '+0x48'): 'IDirectDraw4',
    ('dmp3d', '+0x61c'): 'IDirect3D3',
    ('sound', '+0x1b8'): 'IDirectSoundBuffer',
    ('sound', '+0x1bc'): 'IDirectSound3DBuffer',
    ('sound', '+0x134'): 'IDirectSound3DListener',
    ('dinput', '+0x0@edi'): 'IDirectInputA',
    ('dinput', '+0x0@esi'): 'IDirectInputDeviceA',
    ('dinput', '+0x4'): 'IDirectInputDeviceA',
}
IUNK = ['QueryInterface', 'AddRef', 'Release']
SDK_ORDER = {   # transcribed (DirectX 6 SDK dsound.h / dinput.h), not in win32hle
    'IDirectSound3DBuffer': IUNK + [
        'GetAllParameters', 'GetConeAngles', 'GetConeOrientation', 'GetConeOutsideVolume',
        'GetMaxDistance', 'GetMinDistance', 'GetMode', 'GetPosition', 'GetVelocity',
        'SetAllParameters', 'SetConeAngles', 'SetConeOrientation', 'SetConeOutsideVolume',
        'SetMaxDistance', 'SetMinDistance', 'SetMode', 'SetPosition', 'SetVelocity'],
    'IDirectSound3DListener': IUNK + [
        'GetAllParameters', 'GetDistanceFactor', 'GetDopplerFactor', 'GetOrientation',
        'GetPosition', 'GetRolloffFactor', 'GetVelocity', 'SetAllParameters',
        'SetDistanceFactor', 'SetDopplerFactor', 'SetOrientation', 'SetPosition',
        'SetRolloffFactor', 'SetVelocity', 'CommitDeferredSettings'],
    'IDirectInputA': IUNK + ['CreateDevice', 'EnumDevices', 'GetDeviceStatus', 'RunControlPanel',
                             'Initialize'],
    'IDirectInputDeviceA': IUNK + [
        'GetCapabilities', 'EnumObjects', 'GetProperty', 'SetProperty', 'Acquire', 'Unacquire',
        'GetDeviceState', 'GetDeviceData', 'SetDataFormat', 'SetEventNotification',
        'SetCooperativeLevel', 'GetObjectInfo', 'GetDeviceInfo', 'RunControlPanel', 'Initialize'],
}
# win32hle table name -> the interface name used here (one surface vtable serves 1-4)
HLE_ALIAS = {'IDirectDrawSurface': 'IDirectDrawSurface4', 'IDirectDrawSurface2': 'IDirectDrawSurface4',
             'IDirectDrawSurface3': 'IDirectDrawSurface4'}
ENTRY_RE = re.compile(r'\{\s*"(I[A-Za-z0-9]+)::([A-Za-z0-9_]+)"\s*,\s*([A-Za-z0-9_]+)\s*\}')
STUB_RE = re.compile(r'^(ok\d*|sf_ok_\d+|.*_ok_?\d*|hle_com_(AddRef|QueryInterface)|e_notimpl.*|notimpl.*)$')


def hle_vtables(hle_dir):
    tables = collections.defaultdict(list)
    for fn in ('d3d.c', 'ddraw.c', 'dsound.c'):
        src = open(os.path.join(hle_dir, fn), encoding='utf-8', errors='replace').read()
        for iface, meth, impl in ENTRY_RE.findall(src):
            tables[HLE_ALIAS.get(iface, iface)].append((meth, impl))
    return tables


def field_of(src):
    m = re.match(r'\[(e[a-z]{2})([+-]0x[0-9a-f]+)\]', src)
    if not m:
        return None, None
    return m.group(1), m.group(2)


def main():
    calls, hle_dir, out = sys.argv[1:4]
    tables = hle_vtables(hle_dir)
    used = collections.defaultdict(collections.Counter)
    funcs = collections.defaultdict(set)
    unattributed = collections.defaultdict(collections.Counter)
    for r in csv.DictReader(open(calls), delimiter='\t'):
        fa = int(r['function'], 16)
        rng = next((k for k, (lo, hi) in RANGES.items() if lo <= fa < hi), None)
        if not rng:
            continue
        reg, fld = field_of(r['object_source'])
        iface = FIELDS.get((rng, '%s@%s' % (fld, reg))) or FIELDS.get((rng, fld))
        slot = int(r['slot'])
        if iface:
            used[iface][slot] += 1
            funcs[iface].add(fa)
        else:
            unattributed[rng][slot] += 1
    lines = ['# DirectX methods the game calls (static, provisional)', '',
             'Generated by `scripts/dx_inventory.py` from `results/com_calls.tsv`. "win32hle" says',
             'whether pcrecomp\'s runtime/win32hle answers the slot: `impl` = its own function,',
             '`stub` = returns success without doing anything, `-` = interface absent.', '']
    for iface in sorted(used):
        hle = tables.get(iface)
        names = [m for m, _ in hle] if hle else SDK_ORDER.get(iface, [])
        src = 'win32hle vtable' if hle else 'SDK order (transcribed)'
        lines.append('## %s  (%d call sites in %d functions; names from %s)' % (
            iface, sum(used[iface].values()), len(funcs[iface]), src))
        lines.append('')
        lines.append('| slot | method | sites | win32hle |')
        lines.append('|---:|---|---:|---|')
        for slot, n in sorted(used[iface].items()):
            meth = names[slot] if slot < len(names) else '?'
            if hle and slot < len(hle):
                status = 'stub' if STUB_RE.match(hle[slot][1]) and slot > 2 else 'impl'
            else:
                status = '-'
            lines.append('| %d | %s | %d | %s |' % (slot, meth, n, status))
        lines.append('')
    lines.append('## Unattributed call sites (field fits several interfaces)')
    lines.append('')
    for rng, c in sorted(unattributed.items()):
        lines.append('- %s: slots %s' % (rng, ', '.join('%d(x%d)' % kv for kv in sorted(c.items()))))
    open(out, 'w').write('\n'.join(lines) + '\n')
    print('\n'.join(lines))


if __name__ == '__main__':
    main()
