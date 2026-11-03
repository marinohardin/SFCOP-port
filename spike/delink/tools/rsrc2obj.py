#!/usr/bin/env python3
"""Carry the original .rsrc section into a COFF object that link.exe places as the resource directory.

The section bytes are copied unchanged except for each IMAGE_RESOURCE_DATA_ENTRY.OffsetToData
(an RVA): it becomes the offset inside the section, with an IMAGE_REL_I386_DIR32NB relocation
against the section symbol, so the linker writes the new RVA. This is what cvtres.exe would
produce from a .res file; the toolchain's link.exe cannot spawn cvtres under wibo.

Usage: rsrc2obj.py <exe> <out.obj>
"""
import struct
import sys

import pecoff

def main(exe, out):
    pe = pecoff.PE.load(exe)
    rva, size = pe.dirs[2]
    sec = pe.section_of_rva(rva)
    assert sec['va'] == rva, 'resource directory does not start its section'
    data = bytearray(pe.read(rva, size))
    relocs = []

    def walk(off, depth):
        _, _, _, _, nnamed, nid = struct.unpack_from('<IIHHHH', data, off)
        for i in range(nnamed + nid):
            _, child = struct.unpack_from('<II', data, off + 16 + 8 * i)
            if child & 0x80000000:
                walk(child & 0x7FFFFFFF, depth + 1)
            else:
                target = struct.unpack_from('<I', data, child)[0]
                assert rva <= target < rva + size, 'resource data outside the directory'
                struct.pack_into('<I', data, child, target - rva)
                relocs.append((child, 0, pecoff.IMAGE_REL_I386_DIR32NB))

    walk(0, 0)
    relocs.sort()
    # CNT_INITIALIZED_DATA | ALIGN_4BYTES | MEM_READ, as cvtres emits for .rsrc$01
    pecoff.write_coff_object(out, [('.rsrc$01', 0x40300040, bytes(data), relocs)])
    print('%s: %d bytes, %d data entries relocated' % (out, size, len(relocs)))


if __name__ == '__main__':
    main(*sys.argv[1:])
