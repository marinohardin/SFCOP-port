#!/usr/bin/env python3
"""Compares a replaced function in a relinked exe with the original, byte for byte.

The original function is the range LO-HI of the original exe; the replacement is the map's
SYMBOL, read for the same length. Every differing byte must lie in a dword that holds the
address of a data item in both images: ORIGVA=NAME pairs give the item's original address
and its symbol in the map, and the dword must hold ORIGVA+k in the original and
map(NAME)+k in the relinked exe, for the same k.

Usage: compare_function.py <original exe> <relinked exe> <relinked .map> LO-HI SYMBOL <out.txt>
                           [ORIGVA=NAME ...]
Writes the summary to out.txt (offsets and addresses only, no code bytes); exit status 1 on
any difference those pairs do not explain.
"""
import struct
import sys

import pecoff
import validate


def main(orig_path, new_path, map_path, rng, symbol, out_path, *pairs):
    lo, hi = (int(x, 16) for x in rng.split('-'))
    n = hi - lo
    orig = pecoff.PE.load(orig_path)
    new = pecoff.PE.load(new_path)
    syms = validate.public_symbols(map_path)
    at = syms[symbol]
    items = []
    for p in pairs:
        va, name = p.split('=', 1)
        items.append((int(va, 16), syms[name], name))
    a = orig.read(lo - orig.image_base, n)
    b = new.read(at - new.image_base, n)
    diff = [i for i in range(n) if a[i] != b[i]]
    lines = ['original %08x-%08x (%d bytes), replacement %s at %08x' % (lo, hi, n, symbol, at)]
    explained = set()
    # A dword holding an item's address covers the bytes it explains; try every start that
    # covers each differing byte.
    for i in diff:
        if i in explained:
            continue
        for s in range(max(0, i - 3), min(i, n - 4) + 1):
            o = struct.unpack_from('<I', a, s)[0]
            r = struct.unpack_from('<I', b, s)[0]
            hit = [(ova, nva, name) for ova, nva, name in items
                   if 0 <= o - ova < 0x1000 and r == nva + (o - ova)]
            if hit:
                ova, nva, name = hit[0]
                lines.append('  +%02x: %08x -> %08x = %s%+d' % (s, o, r, name, o - ova))
                explained.update(range(s, s + 4))
                break
    unexplained = [i for i in diff if i not in explained]
    lines.append('bytes equal: %d of %d; differing: %d, all in the dwords above: %s'
                 % (n - len(diff), n, len(diff), 'yes' if not unexplained else 'no'))
    for i in unexplained:
        lines.append('  unexplained difference at +%02x' % i)
    with open(out_path, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print('\n'.join(lines))
    return 1 if unexplained else 0


if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:]))
