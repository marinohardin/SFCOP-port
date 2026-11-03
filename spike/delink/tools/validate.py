#!/usr/bin/env python3
"""Byte-level check of a relinked exe against the original.

Every byte the delinked object carried is found in the relinked image through the link map
(one contribution per section, so each original range moves by a constant, except that a
function excluded with --exclude leaves a hole the rest of its section closes up). Then:

  * a byte outside every relocation must be unchanged;
  * an absolute relocation must hold map(target) + addend, where target + addend is what the
    original held; a PC-relative one must hold map(target) + addend - map(place);
  * a CALL or JMP rel32 that no relocation covers and that crosses a hole is a missed
    relocation (inside one piece it is harmless: both ends move together);
  * an aligned dword in .rdata/.data (or in a data unit inside .text) whose original value is
    an address in the image, but which no relocation covers, is a candidate missed pointer.
    It still points into the old image after the move.

Usage: validate.py <original exe> <relinked exe> <relinked .map> <relocations.tsv> <out dir>
                   [--exclude LO-HI=SYMBOL ...]
  LO-HI is the excluded function's original range (hex, HI exclusive) and SYMBOL the name the
  replacement defines in the map.
Writes <out>/validate.txt (summary) and <out>/stale_candidates.tsv.
"""
import bisect
import os
import re
import struct
import sys

import pecoff

# Ranges the object carried (keep in step with delink.sh) and the section each landed in.
RANGES = [
    (0x00401000, 0x00795000, '.text'),
    (0x007955b8, 0x00835218, '.rdata'),
    (0x00838000, 0x00c83000, '.data'),
    (0x00c83000, 0x00c84000, '.tls'),
]


def contributions(mapfile):
    """Start RVA of the delinked object's contribution to each section, from the map."""
    text = open(mapfile, encoding='latin-1').read()
    secs = {}
    for m in re.finditer(r'^ (\w{4}):(\w{8}) (\w{8})H (\S+)\s+(\w+)$', text, re.M):
        secs.setdefault(int(m.group(1), 16), []).append((int(m.group(2), 16), m.group(4)))
    return secs


def public_symbols(mapfile):
    """Name -> VA for every public symbol in the link map."""
    syms = {}
    for m in re.finditer(r'^ \w{4}:\w{8}\s+(\S+)\s+([0-9a-f]{8}) ', open(mapfile, encoding='latin-1').read(), re.M):
        syms[m.group(1)] = int(m.group(2), 16)
    return syms


def text_like(v):
    """Three low bytes printable: most likely part of a string or a FourCC, not an address."""
    return all(0x20 <= (v >> (8 * k)) & 0xff < 0x7f for k in range(3))


def main(orig_path, new_path, map_path, relocs_path, outdir, *opts):
    holes = []   # (lo, hi, symbol)
    for o in opts:
        if o == '--exclude':
            continue
        rng, sym = o.split('=')
        lo, hi = (int(x, 16) for x in rng.split('-'))
        holes.append((lo, hi, sym))
    orig = pecoff.PE.load(orig_path)
    new = pecoff.PE.load(new_path)
    os.makedirs(outdir, exist_ok=True)
    secs = contributions(map_path)
    # The import address table is not in the object: the linker builds a new one, so a
    # relocation against an IAT slot is checked against the new slot of the same import.
    syms = public_symbols(map_path)
    # section number -> RVA of the section in the new image
    new_sec_rva = {i + 1: s['va'] for i, s in enumerate(new.sections)}
    starts = {}
    for num, items in secs.items():
        for off, name in items:
            starts[name] = new_sec_rva[num] + off
    moves = []   # (orig_lo, orig_hi, delta)
    for lo, hi, name in RANGES:
        delta = new.image_base + starts[name] - lo
        cut = lo
        for hlo, hhi, _ in sorted(holes):
            if lo <= hlo < hi:
                moves.append((cut, hlo, delta))
                delta -= hhi - hlo
                cut = hhi
        moves.append((cut, hi, delta))
    los = [m[0] for m in moves]

    def piece(v):
        i = bisect.bisect_right(los, v) - 1
        return i if i >= 0 and moves[i][0] <= v < moves[i][1] else None

    def mapva(v):
        for hlo, hhi, sym in holes:
            if hlo <= v < hhi:
                return syms[sym] + v - hlo
        i = bisect.bisect_right(los, v) - 1
        if i >= 0 and moves[i][0] <= v <= moves[i][1]:   # one past the end maps too
            return v + moves[i][2]
        return None

    relocs = {}
    for line in open(relocs_path):
        f = line.rstrip('\n').split('\t')
        if f[0] == 'address':
            continue
        relocs[int(f[0], 16)] = (f[1], int(f[2]), int(f[3], 16), int(f[4]), f[5])

    def orig_bytes(va, n):
        return orig.read(va - orig.image_base, n)

    def new_bytes(va, n):
        return new.read(va - new.image_base, n)

    lo_img, hi_img = orig.image_base, orig.image_base + orig.size_of_image
    bad_reloc, changed, stale, crossing = [], [], [], []
    nabs = nrel = 0
    rel_targets = {r[2] for r in relocs.values() if r[0] == 'relpc'}
    for lo, hi, delta in moves:
        ob = orig_bytes(lo, hi - lo)
        nb = new_bytes(lo + delta, hi - lo)
        covered = bytearray(hi - lo)
        for va in range(lo, hi):
            r = relocs.get(va)
            if r is None:
                continue
            kind, width, target, addend, sym = r
            for k in range(width):
                if va - lo + k < len(covered):
                    covered[va - lo + k] = 1
            if kind == 'relpc':
                nrel += 1
                fmt = '<i' if width == 4 else '<b'
                o = struct.unpack_from(fmt, ob, va - lo)[0]
                n = struct.unpack_from(fmt, nb, va - lo)[0]
                want = mapva(target)
                if o != target + addend - va:
                    bad_reloc.append((va, 'original holds %+d, table says %08x%+d' % (o, target, addend), sym))
                elif want is None:
                    bad_reloc.append((va, 'target %08x outside every moved range' % target, sym))
                elif n != want + addend - (va + delta):
                    bad_reloc.append((va, 'relinked holds %+d, expected %+d' % (n, want + addend - va - delta), sym))
                continue
            nabs += 1
            o = struct.unpack_from('<I', ob, va - lo)[0]
            n = struct.unpack_from('<I', nb, va - lo)[0]
            want = syms.get(sym) if sym.startswith('__imp_') else mapva(target)
            if o != (target + addend) & 0xffffffff:
                bad_reloc.append((va, 'original holds %08x, table says %08x+%d' % (o, target, addend), sym))
            elif want is None:
                bad_reloc.append((va, 'target %08x outside every moved range' % target, sym))
            elif n != (want + addend) & 0xffffffff:
                bad_reloc.append((va, 'relinked holds %08x, expected %08x' % (n, want + addend), sym))
        if holes and lo < 0x00795000:
            here = piece(lo)
            for i in range(hi - lo - 4):
                if covered[i] or ob[i] not in (0xe8, 0xe9) or any(covered[i + 1:i + 5]):
                    continue
                t = lo + i + 5 + struct.unpack_from('<i', ob, i + 1)[0]
                if t in rel_targets and piece(t) != here:
                    crossing.append((lo + i, t))
        for i in range(hi - lo):
            if not covered[i] and ob[i] != nb[i]:
                changed.append(lo + i)
        # candidate missed pointers: aligned dwords in data ranges
        if lo >= 0x00795000:
            for i in range(0, hi - lo - 3, 4):
                if covered[i]:
                    continue
                v = struct.unpack_from('<I', ob, i)[0]
                if lo_img + 0x1000 <= v < hi_img and not any(covered[i:i + 4]):
                    stale.append((lo + i, v))
    with open(os.path.join(outdir, 'stale_candidates.tsv'), 'w') as f:
        f.write('address\tvalue\ttext_like\n')
        for va, v in stale:
            f.write('%08x\t%08x\t%d\n' % (va, v, text_like(v)))
    with open(os.path.join(outdir, 'validate.txt'), 'w') as f:
        def out(s):
            print(s)
            f.write(s + '\n')
        for lo, hi, delta in moves:
            out('moved %08x-%08x by %+#x' % (lo, hi, delta))
        out('absolute relocations checked: %d, PC-relative: %d' % (nabs, nrel))
        out('relocations wrong: %d' % len(bad_reloc))
        for va, why, sym in bad_reloc[:20]:
            out('  %08x %s (%s)' % (va, why, sym))
        out('bytes changed outside relocations: %d' % len(changed))
        for va in changed[:20]:
            out('  %08x' % va)
        if holes:
            out('unrelocated CALL/JMP rel32 to a known target across a hole: %d' % len(crossing))
            for va, t in crossing[:20]:
                out('  %08x -> %08x' % (va, t))
        out('aligned data dwords that look like image addresses but have no relocation: %d '
            '(%d of them text-like)' % (len(stale), sum(text_like(v) for _, v in stale)))
    return 0 if not bad_reloc and not changed and not crossing else 1


if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:]))
