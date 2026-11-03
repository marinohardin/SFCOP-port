#!/usr/bin/env python3
"""Lists an i386 COFF object: sections with their relocations, then the external symbols.
Code sections are printed as hex so a compiled function can be compared with the original.

Usage: coffsyms.py <file.obj>
"""
import struct
import sys

REL_TYPES = {6: 'DIR32', 7: 'DIR32NB', 0x14: 'REL32'}


def read_object(data):
    _, nsec, _, symptr, nsym = struct.unpack_from('<HHIII', data, 0)
    strtab = symptr + 18 * nsym

    def name_of(raw):
        if raw[:4] == b'\0\0\0\0':
            off = struct.unpack_from('<I', raw, 4)[0]
            return data[strtab + off:data.index(b'\0', strtab + off)].decode('latin-1')
        return raw.rstrip(b'\0').decode('latin-1')

    symbols = []
    i = 0
    while i < nsym:
        raw = data[symptr + 18 * i:symptr + 18 * i + 18]
        value, secnum, typ, cls, naux = struct.unpack_from('<IhHBB', raw, 8)
        symbols.append((i, name_of(raw[:8]), value, secnum, cls))
        i += 1 + naux
    by_index = {s[0]: s for s in symbols}
    sections = []
    for k in range(nsec):
        h = 20 + 40 * k
        name = name_of(data[h:h + 8])
        if name.startswith('/'):
            off = int(name[1:])
            name = data[strtab + off:data.index(b'\0', strtab + off)].decode('latin-1')
        size, rawptr, relptr, _, nrel, _, chars = struct.unpack_from('<IIIIHHI', data, h + 16)
        relocs = []
        first = 0
        if chars & 0x01000000 and nrel == 0xffff:
            # IMAGE_SCN_LNK_NRELOC_OVFL: the real count is in the first entry's address field
            nrel = struct.unpack_from('<I', data, relptr)[0]
            first = 1
        for r in range(first, nrel):
            va, symi, typ = struct.unpack_from('<IIH', data, relptr + 10 * r)
            relocs.append((va, REL_TYPES.get(typ, hex(typ)), by_index[symi][1]))
        sections.append((k + 1, name, size, chars, data[rawptr:rawptr + size] if rawptr else b'', relocs))
    return sections, symbols


def main(path):
    sections, symbols = read_object(open(path, 'rb').read())
    for num, name, size, chars, raw, relocs in sections:
        if name.startswith('.debug') or name == '.drectve':
            continue
        print('section %d %s size %d flags %08x' % (num, name, size, chars))
        if chars & 0x20:
            print('  bytes', raw.hex(' '))
        for va, typ, sym in relocs:
            print('  reloc +%04x %s %s' % (va, typ, sym))
    for _, name, value, secnum, cls in symbols:
        if cls == 2:
            print('extern %s %s' % ('defined in section %d +%04x' % (secnum, value) if secnum > 0 else 'undefined', name))


if __name__ == '__main__':
    main(sys.argv[1])
