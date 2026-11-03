#!/usr/bin/env python3
"""Copies the one section that defines a function out of a compiled object into a new object.

A translation unit compiled from the SDK also carries its headers' static initializers and
template instances, which the delinked object already has. Keeping only the function's own
section (bytes and relocations, unchanged) lets it replace its delinked copy without
duplicate or extra symbols.

Usage: extract_function.py <in.obj> <mangled name> <out.obj>
"""
import struct
import sys

import coffsyms
import pecoff

TYPES = {v: k for k, v in coffsyms.REL_TYPES.items()}


def main(src, name, dst):
    sections, symbols = coffsyms.read_object(open(src, 'rb').read())
    defs = [s for s in symbols if s[1] == name and s[3] > 0]
    if not defs:
        sys.exit('%s: %s is not defined' % (src, name))
    _, _, value, secnum, _ = defs[0]
    num, secname, size, chars, raw, relocs = sections[secnum - 1]
    # Drop LNK_COMDAT: in the new object the function is an ordinary definition.
    chars &= ~0x1000
    extern = [name] + sorted({sym for _, _, sym in relocs if sym != name})
    index = {n: 2 + i for i, n in enumerate(extern)}
    new_relocs = [(off, index[sym], TYPES[typ]) for off, typ, sym in relocs]
    syms = [(name, value, 1, 2)] + [(n, 0, 0, 2) for n in extern[1:]]
    pecoff.write_coff_object(dst, [(secname, chars, raw, new_relocs)], syms)
    print('%s: %s, %d bytes, %d relocations, needs %s' % (dst, name, size, len(relocs), ', '.join(extern[1:]) or '-'))


if __name__ == '__main__':
    main(*sys.argv[1:])
