#!/usr/bin/env python3
"""Lift StarFleetOP.exe with pcrecomp's lift32 and measure the result.

Runs inside the sandbox container (scripts/sandbox.sh), with work/lifter at /w:

    sandbox.sh python -I /s/run_lift.py [--all | --max N] [--out out/gen]

Adapted from redalert2-recomp's run_lift.py (the reference driver for an MSVC 6
PE32 on pcrecomp): catalog from disasm32 + RTTI vtable seeds, extents by
walking branches (generate.true_extent), outside branch targets added as
entries until none are new, lift32 with precise carry, landing pads for
functions with C++ EH frames (eh32.resume_points). Differences: no game
patches, no hooks, no name_lift; --mark VAs call the host's sfcop_mark(va) on
entry (default: WinMain); and it writes measurements:

    <out>/../lift_stats.json   counts, errors, unimplemented mnemonics, ICALL kinds
    <out>/../lift_errors.txt   one line per lifter exception
"""
import argparse
import collections
import json
import os
import re
import sys
import time

TOOLS = os.environ.get('PCRECOMP_TOOLS', '/w/pcrecomp/tools')
sys.path.insert(0, os.path.join(TOOLS, 'lift'))
sys.path.insert(0, os.path.join(TOOLS, 'pe'))
sys.path.insert(0, os.path.join(TOOLS, 'disasm'))

import pefile                                              # noqa: E402
from capstone import Cs, CS_ARCH_X86, CS_MODE_32          # noqa: E402
import eh32                                                # noqa: E402
from generate import (EXTENT_REACH, closure, find_splits, true_extent,  # noqa: E402
                      linear_disassemble_function, lift_function_linear, write_chunk)
from lift32 import Lifter                                  # noqa: E402
from pe_analyze import analyze_pe, build_iat_map           # noqa: E402

UNIMPL_RE = re.compile(r'UNIMPLEMENTED: (\S+)')
ICALL_RE = re.compile(r'RECOMP_ICALL\(([^;]*?)\);')


def icall_kind(arg):
    """Classify a RECOMP_ICALL operand as the lifter wrote it."""
    if arg.startswith('MEM32(') and ('g_e' in arg or re.search(r'\be[a-ds][xip]\b', arg)):
        return 'mem[reg+off] (vtable/fnptr field)'
    if arg.startswith('MEM32('):
        return 'mem[abs] (global fnptr)'
    if arg.startswith('0x'):
        return 'direct target not lifted'
    if arg == '0':
        return 'unresolved'
    return 'register'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default='game/StarFleetOP.exe')
    ap.add_argument('--catalog', default='cat/functions.json')
    ap.add_argument('--seeds', default='cat/rtti_seeds.json')
    ap.add_argument('--out', default='out/gen')
    ap.add_argument('--max', type=int, default=3000, help='closure size cap')
    ap.add_argument('--all', action='store_true', help='lift every catalogued function')
    ap.add_argument('--split', type=int, default=400, help='functions per .c file')
    # WinMain (the CRT entry calls it right after GetModuleHandleA(NULL)).
    ap.add_argument('--mark', default='0x0069A840',
                    help='comma-separated VAs whose entry calls the host\'s sfcop_mark(va)')
    args = ap.parse_args()

    info = analyze_pe(args.exe)
    iat = build_iat_map(info)
    cs, ce = info.code_start, info.code_end
    print('[*] base=0x%08X code=0x%08X-0x%08X IAT=%d' % (info.image_base, cs, ce, len(iat)))
    pe = pefile.PE(args.exe, fast_load=True)
    base = info.image_base

    def read(va, n):
        try:
            return pe.get_data(va - base, n)
        except Exception:                           # noqa: BLE001 -- outside the image
            return None

    cat = json.load(open(args.catalog))
    byaddr = {f['address']: f for f in cat['functions'] if cs <= f['address'] < ce}
    n_catalog = len(byaddr)
    print('[*] catalog: %d functions inside .text' % n_catalog)

    want = set()
    if os.path.exists(args.seeds):
        want = {e['address'] for e in json.load(open(args.seeds))
                if cs <= e['address'] < ce and e['address'] not in byaddr}
    known = sorted(set(byaddr) | want)
    nxt = {a: (known[i + 1] if i + 1 < len(known) else ce) for i, a in enumerate(known)}
    for a in want:
        byaddr[a] = {'address': a, 'end': min(nxt[a], a + 0x100), 'calls_to': [], 'entry_kind': 'start'}
    print('[*] vtable slots not in the catalog: %d injected' % len(want))

    entry = base + info.entry_point_rva
    marks = {int(x, 0) for x in args.mark.split(',') if x.strip()}
    chosen = set(byaddr) if args.all else set(closure(byaddr, [entry], args.max))
    print('[*] lifting %d of %d functions' % (len(chosen), len(byaddr)))

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    text = [s for s in info.sections if s.name == '.text'][0]
    code = open(args.exe, 'rb').read()[text.raw_offset:text.raw_offset + text.raw_size]

    lifter = Lifter(iat_map=iat, lifted=set(byaddr), precise_carry=True, precise_sbb=True)
    os.makedirs(args.out, exist_ok=True)
    for fn in os.listdir(args.out):
        if fn.startswith('recomp_') and fn.endswith('.c'):
            os.remove(os.path.join(args.out, fn))

    entries, chunk, idx, dirty = [], [], 0, 0
    errors = []
    unimpl = collections.Counter()
    unimpl_funcs = collections.Counter()
    icalls = collections.Counter()
    eh = {'frames': 0, 'with_resume': 0, 'resume_points': 0}
    n_insns = 0
    ordered = sorted(byaddr)
    starts = {a for a in ordered if byaddr[a].get('entry_kind') != 'alias'}
    t_split = time.time()
    splits = find_splits(md, code, cs, ce, starts)
    starts -= splits
    print('[*] split entries: %d, %.0fs' % (len(splits), time.time() - t_split))
    t0 = time.time()

    def flush(force=False):
        nonlocal chunk, idx
        if chunk and (force or len(chunk) >= args.split):
            write_chunk(args.out, idx, chunk)
            idx += 1
            chunk = []

    def lift_one(addr, name, end, reached):
        nonlocal n_insns
        try:
            lo = min(reached) if reached else addr
            insns, leaders = (linear_disassemble_function(md, code, cs, lo, end, reached=reached)
                              if end > addr else ([], None))
            if leaders is not None:
                leaders.add(addr)
            resume = None
            if insns:
                head = [i for i in insns if i.address >= addr][:eh32.PROLOGUE_WINDOW]
                resume = eh32.resume_points(md, read, head)
                if resume is not None:
                    eh['frames'] += 1
                    eh['with_resume'] += bool(resume)
                    eh['resume_points'] += len(resume)
            n_insns += len(insns)
            body = (lift_function_linear(lifter, name, insns, leaders, addr, eh_resume=resume) if insns
                    else 'void %s(void) { }\n' % name)
            if addr in marks:
                enter = 'RECOMP_ENTER(0x%08Xu);' % addr
                body = body.replace(enter, enter + ' sfcop_mark(0x%08Xu);' % addr, 1)
        except Exception as e:                      # noqa: BLE001 -- counted, not hidden
            body = '/* ERROR %s: %s */\nvoid %s(void) { }\n' % (name, e, name)
            errors.append('0x%08X\t%s: %s' % (addr, type(e).__name__, e))
        found = UNIMPL_RE.findall(body)
        unimpl.update(found)
        for m in set(found):
            unimpl_funcs[m] += 1
        for arg in ICALL_RE.findall(body):
            icalls[icall_kind(arg.strip())] += 1
        chunk.append((body, addr, name))
        entries.append((addr, name))
        if len(chunk) >= args.split:
            flush()
            print('[*]   %d/%d (%d err) %.0fs' % (len(entries), len(chosen), len(errors), time.time() - t0),
                  flush=True)

    todo, added = sorted(chosen), 0
    while todo:
        outside = set()
        for addr in todo:
            name = 'sub_%08X' % addr
            reached, behind, called = set(), set(), set()
            end, clean = true_extent(md, code, cs, addr, min(addr + EXTENT_REACH, ce), starts,
                                     reached=reached, behind=behind, called=called)
            outside |= {t for t in behind if t not in reached} | called
            dirty += not clean
            lift_one(addr, name, end, reached)
        todo = sorted(t for t in outside if cs <= t < ce and t not in byaddr)
        for t in todo:
            byaddr[t] = {'address': t, 'end': ce, 'calls_to': [], 'entry_kind': 'start'}
            chosen.add(t)
        added += len(todo)
    print('[*] branch/call targets outside the catalog, added: %d' % added)

    stubs = [a for a in ordered if a not in chosen]
    for a in stubs:
        name = 'sub_%08X' % a
        chunk.append(('void %s(void) { RECOMP_NOT_LIFTED(0x%08Xu); }\n' % (name, a), a, name))
        entries.append((a, name))
        flush()
    flush(force=True)

    with open(os.path.join(args.out, 'recomp_funcs.h'), 'w', newline='\n') as f:
        f.write('/* StarFleetOP.exe - generated by spike/lifter/scripts/run_lift.py */\n#pragma once\n'
                '#include <stdint.h>\n\nvoid recomp_not_lifted(uint32_t va);\n'
                '#define RECOMP_NOT_LIFTED(va) recomp_not_lifted(va)\n'
                'void sfcop_mark(uint32_t va);\n\n')
        for a, n in entries:
            f.write('void %s(void);\n' % n)
    with open(os.path.join(args.out, 'recomp_dispatch.c'), 'w', newline='\n') as f:
        f.write('/* StarFleetOP.exe - generated by spike/lifter/scripts/run_lift.py */\n'
                '#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n'
                'const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n')
        for a, n in sorted(entries):
            f.write('    { 0x%08Xu, %s },\n' % (a, n))
        f.write('};\nconst uint32_t recomp_dispatch_count = %d;\n'
                'const uint32_t sfcop_entry_va = 0x%08Xu;\n' % (len(entries), entry))

    lines = nbytes = 0
    for fn in os.listdir(args.out):
        p = os.path.join(args.out, fn)
        nbytes += os.path.getsize(p)
        lines += sum(1 for _ in open(p, encoding='utf-8', errors='replace'))
    stats = {
        'catalog_functions': n_catalog, 'rtti_slots_injected': len(want),
        'outside_targets_added': added, 'lifted': len(chosen), 'not_lifted_stubs': len(stubs),
        'errors': len(errors), 'no_terminator': dirty, 'instructions_lifted': n_insns,
        'files': idx, 'lines': lines, 'bytes': nbytes, 'seconds': round(time.time() - t0, 1),
        'eh_cxx_frames': eh, 'unimplemented_by_mnemonic': dict(unimpl.most_common()),
        'unimplemented_functions_by_mnemonic': dict(unimpl_funcs.most_common()),
        'icall_sites_by_kind': dict(icalls.most_common()),
    }
    parent = os.path.dirname(os.path.abspath(args.out))
    json.dump(stats, open(os.path.join(parent, 'lift_stats.json'), 'w'), indent=1)
    open(os.path.join(parent, 'lift_errors.txt'), 'w').write('\n'.join(errors) + '\n')
    print('=' * 60)
    print('  lifted %d  stubs %d  errors %d  no-terminator %d' % (len(chosen), len(stubs), len(errors), dirty))
    print('  %s lines, %.1f MB of C in %d files, %.1fs' % (format(lines, ','), nbytes / 1e6, idx, stats['seconds']))
    print('  unimplemented: %d sites, %d mnemonics' % (sum(unimpl.values()), len(unimpl)))
    print('=' * 60)


if __name__ == '__main__':
    main()
