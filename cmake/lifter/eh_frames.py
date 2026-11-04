#!/usr/bin/env python3
"""Static map of a VC6 image's exception frames, for the lift and the report.

What the lifted build needs to know before it runs, all read from the image:

  * every C++ EH handler stub (`mov eax, FuncInfo; jmp __CxxFrameHandler`), its
    FuncInfo, try blocks and catch handlers;
  * which function installs each stub (the `push offset stub` in its prologue)
    and the prologue shape (ebp frame, frame-pointer-omitted, other);
  * where each catch funclet returns: the continuation the function resumes at
    after that catch (`mov eax, offset $L; ret` at the funclet's exits);
  * every `__try/__except` frame (`push scopetable; push offset
    _except_handler3`) and its `__except` blocks, which are continuations too;
  * the two CRT routines that move control into a frame, `_JumpToContinuation`
    and the `__except` call inside `_except_handler3`, found by their bytes.

Run on the host (capstone + pefile only):

    eh_frames.py EXE CATALOG.json OUT.json [OUT.tsv]

The build's lift driver (run_lift.py) imports analyse() inside the sandbox container.
"""
import bisect
import json
import re
import struct
import sys

import pefile
from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_REG_AH, X86_REG_AL, X86_REG_AX, X86_REG_EAX

EH_MAGICS = (0x19930520, 0x19930521, 0x19930522)

# The VC6 CRT routines, as the SP4 compiler's static CRT has them (identical in
# StarFleetOP.exe and in a test exe linked with toolchain/'s LIBCMT.LIB).
SIG_JUMP_TO_CONTINUATION = bytes.fromhex(
    '558BEC51535657' '8B450C83C00C8945FC' '648B1D00000000' '8B03' '64A300000000'
    '8B4508' '8B5D0C' '8B63FC' '8B6DFC' 'FFE0')
# __EH_prolog: `mov eax, offset stub; call __EH_prolog` builds the same
# record as the ebp prologue (record at entry-16, ebp at entry-4).
SIG_EH_PROLOG = bytes.fromhex('6AFF5064A100000000508B44240C64892500000000896C240C8D6C240C50C3')
SIG_EXCEPT_HANDLER3 = bytes.fromhex('558BEC83EC0853565755FC8B5D0C8B4508F7400406000000')
# _CxxThrowException(object, ThrowInfo): copies a static EXCEPTION_RECORD
# template, then calls RaiseException. The template's address varies.
SIG_CXX_THROW = re.compile(re.escape(bytes.fromhex('558BEC83EC208B450856576A0859BE')) + b'....'
                           + re.escape(bytes.fromhex('8D7DE0F3A5')), re.DOTALL)

FUNCLET_LIMIT = 4000        # instructions walked per catch funclet
PROLOGUE_BYTES = 0x40       # a frame is installed this close to the function start
EAX_REGS = {X86_REG_EAX, X86_REG_AX, X86_REG_AL, X86_REG_AH}


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        text = [s for s in self.pe.sections if s.Name.rstrip(b'\0') == b'.text'][0]
        self.text_va = self.base + text.VirtualAddress
        self.text = text.get_data()[:text.Misc_VirtualSize]
        self.text_end = self.text_va + len(self.text)
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True

    def read(self, va, n):
        try:
            b = self.pe.get_data(va - self.base, n)
        except Exception:                       # noqa: BLE001 -- outside the image
            return None
        return b if len(b) == n else None

    def u32(self, va):
        b = self.read(va, 4)
        return struct.unpack('<I', b)[0] if b else None

    def i32(self, va):
        b = self.read(va, 4)
        return struct.unpack('<i', b)[0] if b else None

    def in_text(self, va):
        return va is not None and self.text_va <= va < self.text_end

    def find(self, sig):
        return [self.text_va + m.start() for m in re.finditer(re.escape(sig), self.text)]

    def pushes_of(self, imm):
        """VAs of `push imm32` instructions with this immediate."""
        return self.find(b'\x68' + struct.pack('<I', imm))

    def eh_prolog_calls_of(self, imm, target):
        """VAs of `mov eax, imm32; call target` pairs."""
        out = []
        for s in self.find(b'\xB8' + struct.pack('<I', imm) + b'\xE8'):
            rel = struct.unpack('<i', self.read(s + 6, 4))[0]
            if s + 10 + rel == target:
                out.append(s)
        return out

    def disasm(self, va, count):
        code = self.read(va, 16 * count) or b''
        return list(self.md.disasm(code, va, count))


def cxx_stubs(img):
    """{stub VA: FuncInfo VA} for every `mov eax, FuncInfo; jmp ...` stub."""
    out = {}
    for m in re.finditer(rb'\xB8(....)(?:\xE9|\xFF\x25)', img.text, re.DOTALL):
        fi = struct.unpack('<I', m.group(1))[0]
        if img.u32(fi) in EH_MAGICS:
            out[img.text_va + m.start()] = fi
    return out


def funcinfo(img, fi):
    max_state, _unwind, ntry, tries = (img.u32(fi + 4), img.u32(fi + 8), img.u32(fi + 12), img.u32(fi + 16))
    blocks = []
    for t in range(min(ntry or 0, 4096)):
        lo, hi, chi, nc, hp = (img.i32(tries + 20 * t + k) for k in (0, 4, 8, 12, 16))
        catches = []
        for c in range(min(nc or 0, 256)):
            h = (hp & 0xFFFFFFFF) + 16 * c
            adj, ptype, disp, addr = (img.u32(h + k) for k in (0, 4, 8, 12))
            name = None
            if ptype:
                raw = img.read(ptype + 8, 128) or b''
                name = raw.split(b'\0', 1)[0].decode('latin1')
            catches.append({'adjectives': adj, 'type': name or '...', 'disp': disp, 'funclet': addr})
        blocks.append({'try_low': lo, 'try_high': hi, 'catch_high': chi, 'catches': catches})
    return {'max_state': max_state, 'try_blocks': blocks}


def funclet_continuations(img, va, noreturn=(), throws=None):
    """Addresses a catch funclet returns to the frame handler: the value eax
    holds at each `ret` it can reach, when that is an immediate. The optimiser
    schedules other instructions between `mov eax, offset $L` and the `ret`, so
    the immediate is kept until something writes eax. Follows branches (eax
    is tracked along each path); a call to a noreturn routine (the throw
    helper) ends that path and is counted in throws[0]. An address is visited
    once per eax value, so arms that load different immediates and meet at
    one `ret` each give their continuation."""
    seen, out, work = set(), set(), [(va, None)]
    steps = 0
    while work and steps < FUNCLET_LIMIT:
        a, last_imm = work.pop()
        while (a, last_imm) not in seen and steps < FUNCLET_LIMIT:
            seen.add((a, last_imm))
            steps += 1
            ins = img.disasm(a, 1)
            if not ins:
                break
            i = ins[0]
            nxt = i.address + i.size
            mn, op = i.mnemonic, i.op_str
            if mn == 'ret':
                if last_imm is not None:
                    out.add(last_imm)
                break
            if mn == 'int3':
                break
            if mn == 'mov' and op.startswith('eax, 0x') and ',' not in op[5:]:
                last_imm = int(op[5:], 16)
                a = nxt
                continue
            if mn == 'call' or EAX_REGS & set(i.regs_access()[1]):
                last_imm = None
            if mn == 'jmp':
                if op.startswith('0x'):
                    a = int(op, 16)
                    continue
                break                       # indirect: not followed
            if mn.startswith('j') or mn in ('loop', 'loope', 'loopne', 'jecxz'):
                if op.startswith('0x'):
                    work.append((int(op, 16), last_imm))
                a = nxt
                continue
            if mn == 'call' and op.startswith('0x') and int(op, 16) in noreturn:
                if throws is not None:
                    throws[0] += 1
                break
            a = nxt
    return sorted(out)


def at_prologue(img, fva, site):
    """The byte pattern at `site` is an instruction of fva's prologue (a byte
    search also matches inside other instructions)."""
    if site - fva > PROLOGUE_BYTES:
        return False
    return any(i.address == site for i in img.disasm(fva, 24) if i.address <= site)


def prologue_kind(img, fva, stub_site):
    ins = img.disasm(fva, 3)
    if len(ins) >= 2 and ins[0].mnemonic == 'push' and ins[0].op_str == 'ebp' \
            and ins[1].mnemonic == 'mov' and ins[1].op_str == 'ebp, esp':
        kind = 'ebp'
    elif ins and ins[0].mnemonic == 'push' and ins[0].op_str in ('-1', '0xffffffff'):
        kind = 'fpo'
    else:
        kind = 'other'
    return kind, stub_site - fva


def scope_table(img, table):
    """__try scopes of an _except_handler3 frame: (enclosing, filter, handler)."""
    out = []
    for i in range(64):
        enc, flt, hnd = img.i32(table + 12 * i), img.u32(table + 12 * i + 4), img.u32(table + 12 * i + 8)
        if enc is None or not (-1 <= enc < i) or not img.in_text(hnd) or (flt and not img.in_text(flt)):
            break
        out.append({'enclosing': enc, 'filter': flt, 'handler': hnd})
    return out


def analyse(path, starts):
    """starts: sorted function start VAs (the catalogue's, without aliases)."""
    img = Image(path)
    starts = sorted(starts)

    def owner(site):
        k = bisect.bisect_right(starts, site) - 1
        return starts[k] if k >= 0 else None

    crt = {}
    hits = img.find(SIG_JUMP_TO_CONTINUATION)
    crt['jump_to_continuation'] = hits[0] if len(hits) == 1 else None
    hits = img.find(SIG_EXCEPT_HANDLER3)
    crt['except_handler3'] = hits[0] if len(hits) == 1 else None
    crt['except_handler3_call'] = None
    if crt['except_handler3']:
        body = img.disasm(crt['except_handler3'], 120)
        for a, b in zip(body, body[1:]):
            if a.mnemonic == 'mov' and a.op_str == 'dword ptr [ebx + 0xc], eax' \
                    and b.mnemonic == 'call' and b.op_str == 'dword ptr [edi + ecx*4 + 8]':
                crt['except_handler3_call'] = b.address
    hits = [img.text_va + m.start() for m in SIG_CXX_THROW.finditer(img.text)]
    crt['cxx_throw_exception'] = hits[0] if len(hits) == 1 else None
    hits = img.find(SIG_EH_PROLOG)
    crt['eh_prolog'] = hits[0] if len(hits) == 1 else None
    noreturn = {crt['cxx_throw_exception']} - {None}

    stubs = cxx_stubs(img)
    cxx = []
    for stub, fi in sorted(stubs.items()):
        info = funcinfo(img, fi)
        if not info['try_blocks']:
            continue
        for blk in info['try_blocks']:
            for c in blk['catches']:
                throws = [0]
                c['continuations'] = (funclet_continuations(img, c['funclet'], noreturn, throws)
                                      if c['funclet'] else [])
                c['throw_exits'] = throws[0]
        sites = [(s, None) for s in img.pushes_of(stub)]
        if crt['eh_prolog']:
            sites += [(s, 'eh_prolog') for s in img.eh_prolog_calls_of(stub, crt['eh_prolog'])]
        owners = []
        for s, how in sites:
            f = owner(s)
            if f is None or not at_prologue(img, f, s):
                continue
            kind, off = prologue_kind(img, f, s)
            kind = how or kind
            owners.append({'function': f, 'prologue': kind, 'stub_push_offset': off})
        cxx.append({'stub': stub, 'funcinfo': fi, 'owners': owners, **info})

    seh = []
    if crt['except_handler3']:
        for s in img.pushes_of(crt['except_handler3']):
            before = img.read(s - 5, 5)
            table = struct.unpack('<I', before[1:])[0] if before and before[0] == 0x68 else None
            f = owner(s)
            if f is not None and not at_prologue(img, f, s):
                continue
            scopes = scope_table(img, table) if table else []
            kind, off = prologue_kind(img, f, s) if f is not None else ('other', None)
            seh.append({'function': f, 'prologue': kind, 'handler_push_offset': off,
                        'scope_table': table, 'scopes': scopes})
    return {'crt': crt, 'cxx': cxx, 'seh': seh}


def landing_pads(frames):
    """{function VA: set of continuation VAs} for every function a transfer
    into a frame can resume in, and {continuation VA: set of owner VAs}."""
    pads, cont_owner = {}, {}
    for e in frames['cxx']:
        conts = {a for b in e['try_blocks'] for c in b['catches'] for a in c['continuations']}
        for o in e['owners']:
            pads.setdefault(o['function'], set()).update(conts)
            for a in conts:
                cont_owner.setdefault(a, set()).add(o['function'])
    for e in frames['seh']:
        if e['function'] is None:
            continue
        conts = {s['handler'] for s in e['scopes'] if s['filter']}
        pads.setdefault(e['function'], set()).update(conts)
        for a in conts:
            cont_owner.setdefault(a, set()).add(e['function'])
    return pads, cont_owner


def funclets(frames):
    """Code the CRT reaches only by an indirect call into the middle of its function: catch
    funclets (__CxxFrameHandler), `__except` filters (_except_handler3) and `__finally` blocks
    (_local_unwind2)."""
    out = {c['funclet'] for e in frames['cxx'] for b in e['try_blocks'] for c in b['catches']
           if c['funclet']}
    for e in frames['seh']:
        for s in e['scopes']:
            out.add(s['filter'] if s['filter'] else s['handler'])
    return out


def summary(frames):
    cxx = frames['cxx']
    catches = [c for e in cxx for b in e['try_blocks'] for c in b['catches']]
    owners = {o['function'] for e in cxx for o in e['owners']}
    kinds = {}
    for e in cxx:
        for o in e['owners']:
            kinds[o['prologue']] = kinds.get(o['prologue'], 0) + 1
    return {
        'crt': {k: ('0x%08X' % v if v else None) for k, v in frames['crt'].items()},
        'cxx_funcinfo_with_try': len(cxx),
        'cxx_try_blocks': sum(len(e['try_blocks']) for e in cxx),
        'cxx_catch_handlers': len(catches),
        'cxx_catches_with_continuation': sum(1 for c in catches if c['continuations']),
        'cxx_catches_always_rethrow': sum(1 for c in catches if not c['continuations'] and c['throw_exits']),
        'cxx_catches_without_continuation': ['0x%08X' % c['funclet'] for c in catches
                                             if not c['continuations'] and not c['throw_exits']],
        'cxx_owner_functions': len(owners),
        'cxx_stubs_without_owner': ['0x%08X' % e['stub'] for e in cxx if not e['owners']],
        'cxx_owner_prologues': kinds,
        'seh_frames': len(frames['seh']),
        'seh_except_blocks': sum(1 for e in frames['seh'] for s in e['scopes'] if s['filter']),
        'seh_finally_blocks': sum(1 for e in frames['seh'] for s in e['scopes'] if not s['filter']),
    }


def write_tsv(frames, path):
    """One row per function that owns a catching frame: what resumes where."""
    with open(path, 'w') as f:
        f.write('function\tkind\tprologue\tframe\tcatches\tcontinuations\n')
        for e in frames['cxx']:
            catches = [c for b in e['try_blocks'] for c in b['catches']]
            conts = sorted({a for c in catches for a in c['continuations']})
            types = ','.join(c['type'] + ('' if c['continuations'] else '(rethrows)') for c in catches)
            for o in e['owners']:
                f.write('0x%08X\tc++\t%s\t0x%08X\t%s\t%s\n' % (
                    o['function'], o['prologue'], e['funcinfo'], types, ','.join('0x%08X' % a for a in conts)))
        for e in frames['seh']:
            conts = [s['handler'] for s in e['scopes'] if s['filter']]
            f.write('0x%08X\tseh\t%s\t0x%08X\t%d __except, %d __finally\t%s\n' % (
                e['function'] or 0, e['prologue'], e['scope_table'] or 0, len(conts),
                sum(1 for s in e['scopes'] if not s['filter']), ','.join('0x%08X' % a for a in conts)))


def main():
    """eh_frames.py EXE CATALOG.json OUT.json [OUT.tsv]"""
    exe, catalog, out = sys.argv[1:4]
    cat = json.load(open(catalog))
    starts = [f['address'] for f in cat['functions'] if f.get('entry_kind') != 'alias']
    frames = analyse(exe, starts)
    json.dump({'summary': summary(frames), 'frames': frames}, open(out, 'w'), indent=1)
    if len(sys.argv) > 4:
        write_tsv(frames, sys.argv[4])
    print(json.dumps(summary(frames), indent=1))


if __name__ == '__main__':
    main()
