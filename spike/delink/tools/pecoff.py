"""Small PE, COFF archive and short-import readers for the delink spike (stdlib only)."""
import struct


class PE:
    """Just enough of a 32-bit PE image: headers, sections, data directories, imports."""

    def __init__(self, data):
        self.data = data
        pe = struct.unpack_from('<I', data, 0x3C)[0]
        self.pe = pe
        (self.machine, self.nsections, self.timestamp, _, _, self.optsize,
         self.characteristics) = struct.unpack_from('<HHIIIHH', data, pe + 4)
        o = pe + 24
        self.opt = o
        self.entry = struct.unpack_from('<I', data, o + 16)[0]
        self.image_base = struct.unpack_from('<I', data, o + 28)[0]
        self.size_of_image = struct.unpack_from('<I', data, o + 56)[0]
        self.subsystem = struct.unpack_from('<H', data, o + 68)[0]
        self.dirs = [struct.unpack_from('<II', data, o + 96 + 8 * i) for i in range(16)]
        self.sections = []
        s = o + self.optsize
        for i in range(self.nsections):
            name, vsize, va, rsize, rptr = struct.unpack_from('<8sIIII', data, s + 40 * i)
            chars = struct.unpack_from('<I', data, s + 40 * i + 36)[0]
            self.sections.append(dict(name=name.rstrip(b'\0').decode(), vsize=vsize, va=va,
                                      rsize=rsize, rptr=rptr, chars=chars))

    @classmethod
    def load(cls, path):
        with open(path, 'rb') as f:
            return cls(f.read())

    def section(self, name):
        for s in self.sections:
            if s['name'] == name:
                return s
        return None

    def section_of_rva(self, rva):
        for s in self.sections:
            if s['va'] <= rva < s['va'] + max(s['vsize'], s['rsize']):
                return s
        return None

    def off(self, rva):
        s = self.section_of_rva(rva)
        if s is None:
            raise ValueError('rva %#x outside every section' % rva)
        return s['rptr'] + rva - s['va']

    def read(self, rva, n):
        """Bytes at an RVA; zero fill past the raw data (bss)."""
        s = self.section_of_rva(rva)
        o = rva - s['va']
        raw = self.data[s['rptr'] + o: s['rptr'] + min(s['rsize'], o + n)] if o < s['rsize'] else b''
        return raw + b'\0' * (n - len(raw))

    def u32(self, rva):
        return struct.unpack('<I', self.read(rva, 4))[0]

    def cstr(self, rva):
        o = self.off(rva)
        return self.data[o:self.data.index(b'\0', o)].decode('latin-1')

    def imports(self):
        """[(dll, [(name or None, ordinal or None, iat_rva)])] in descriptor order."""
        va, _ = self.dirs[1]
        out = []
        while True:
            ilt, _, _, name, iat = struct.unpack('<IIIII', self.read(va, 20))
            if not name:
                break
            entries = []
            src, slot = (ilt or iat), iat
            while True:
                v = self.u32(src)
                if not v:
                    break
                if v & 0x80000000:
                    entries.append((None, v & 0xFFFF, slot))
                else:
                    entries.append((self.cstr(v + 2), None, slot))
                src += 4
                slot += 4
            out.append((self.cstr(name), entries))
            va += 20
        return out


def archive_members(data):
    """Yield (name, offset_of_body, body) for each member of a COFF archive."""
    assert data[:8] == b'!<arch>\n', 'not an archive'
    pos = 8
    longnames = b''
    while pos + 60 <= len(data):
        hdr = data[pos:pos + 60]
        name = hdr[:16].decode('latin-1').rstrip()
        size = int(hdr[48:58].decode().strip())
        body = data[pos + 60: pos + 60 + size]
        if name == '//':
            longnames = body
        elif name.startswith('/') and name[1:].isdigit():
            o = int(name[1:])
            end = longnames.find(b'\0', o)
            name = longnames[o:end if end >= 0 else None].decode('latin-1').rstrip('/\n')
        yield name, pos + 60, body
        pos += 60 + size + (size & 1)


IMPORT_NAME_TYPES = {0: 'ordinal', 1: 'name', 2: 'noprefix', 3: 'undecorate'}


def short_imports(data):
    """Parse every short import member of an import library.

    Returns a list of dicts: symbol (public name, e.g. _GetProcAddress@8), dll, name_type,
    ordinal_hint, kind (0 code, 1 data, 2 const) and import (the name or ordinal the loader sees).
    """
    out = []
    for name, _, body in archive_members(data):
        if len(body) < 20:
            continue
        sig1, sig2 = struct.unpack_from('<HH', body, 0)
        if sig1 != 0 or sig2 != 0xFFFF:
            continue
        _, machine, _, size, hint, typ = struct.unpack_from('<HHIIHH', body, 4)
        strs = body[20:20 + size].split(b'\0')
        sym, dll = strs[0].decode('latin-1'), strs[1].decode('latin-1')
        kind, ntype = typ & 3, (typ >> 2) & 7
        if ntype == 0:
            imp = hint
        elif ntype == 1:
            imp = sym
        elif ntype == 2:
            imp = sym[1:] if sym[:1] in '?@_' else sym
        else:
            imp = (sym[1:] if sym[:1] in '?@_' else sym).split('@')[0]
        out.append(dict(symbol=sym, dll=dll, name_type=IMPORT_NAME_TYPES[ntype],
                        ordinal_hint=hint, kind=kind, imp=imp))
    return out


IMAGE_REL_I386_DIR32 = 0x06
IMAGE_REL_I386_DIR32NB = 0x07
IMAGE_REL_I386_REL32 = 0x14


def write_coff_object(path, sections, symbols=()):
    """Write a minimal i386 COFF object.

    sections: [(name, characteristics, data, [(offset, symbol_index, type)])]; section i gets
    the section symbol at index 2*i (each section symbol has one aux record).
    symbols: extra [(name, value, section_number (1-based, 0 = undefined), storage_class)]
    appended after the section symbols; their indexes follow 2*len(sections).
    """
    strtab = bytearray(4)

    def name_field(name):
        b = name.encode('latin-1')
        if len(b) <= 8:
            return b.ljust(8, b'\0')
        off = len(strtab)
        strtab.extend(b + b'\0')
        return b'\0\0\0\0' + struct.pack('<I', off)

    nsec = len(sections)
    header_size = 20 + 40 * nsec
    pos = header_size
    layout = []
    for name, chars, data, relocs in sections:
        dpos = pos
        pos += len(data)
        rpos = pos
        pos += 10 * len(relocs)
        layout.append((dpos, rpos))
    symtab_pos = pos

    syms = bytearray()
    for i, (name, chars, data, relocs) in enumerate(sections):
        syms += name_field(name) + struct.pack('<IhHBB', 0, i + 1, 0, 3, 1)
        syms += struct.pack('<IHHIHB3x', len(data), len(relocs), 0, 0, 0, 0)
    for name, value, secnum, sclass in symbols:
        syms += name_field(name) + struct.pack('<IhHBB', value, secnum, 0x20 if sclass == 2 and secnum else 0,
                                               sclass, 0)
    nsyms = len(syms) // 18
    struct.pack_into('<I', strtab, 0, len(strtab))

    out = bytearray(struct.pack('<HHIIIHH', 0x14C, nsec, 0, symtab_pos, nsyms, 0, 0))
    for (name, chars, data, relocs), (dpos, rpos) in zip(sections, layout):
        out += name_field(name) + struct.pack('<IIIIIIHHI', 0, 0, len(data), dpos if data else 0,
                                              rpos if relocs else 0, 0, len(relocs), 0, chars)
    for name, chars, data, relocs in sections:
        out += data
        for off, sym, typ in relocs:
            out += struct.pack('<IIH', off, sym, typ)
    assert len(out) == symtab_pos
    out += syms + strtab
    with open(path, 'wb') as f:
        f.write(out)
