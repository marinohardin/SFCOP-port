"""A 32-bit PE DLL as scr2native reads it: headers, sections, base relocations, imports, exports.

Standard library only. Nothing here runs or trusts the DLL: every offset is checked against the
file before it is read, and a file that breaks the format raises PeError naming what is wrong.
"""
import hashlib
import struct

IMAGE_FILE_MACHINE_I386 = 0x014C
IMAGE_FILE_DLL = 0x2000
PE32_MAGIC = 0x010B
REL_ABSOLUTE = 0
REL_HIGHLOW = 3


class PeError(Exception):
    """The file is not a 32-bit PE DLL this tool can read. The message says why."""


class Section:
    def __init__(self, name, rva, vsize, raw_offset, raw_size, characteristics):
        self.name = name
        self.rva = rva
        self.vsize = vsize
        self.raw_offset = raw_offset
        self.raw_size = raw_size
        self.characteristics = characteristics

    @property
    def size(self):
        """The section's extent in the image: its virtual size, or its raw size if that is 0."""
        return self.vsize or self.raw_size

    @property
    def executable(self):
        return bool(self.characteristics & 0x20000020)  # code, or execute

    def __repr__(self):
        return 'Section(%s, rva=0x%X, size=0x%X)' % (self.name, self.rva, self.size)


class Import:
    def __init__(self, dll, name, iat_rva):
        self.dll = dll          # upper case, as the allow-list writes it: KERNEL32.DLL
        self.name = name        # the function's name, or '#<ordinal>'
        self.iat_rva = iat_rva  # the slot the loader fills with the function's address

    @property
    def full(self):
        """The allow-list's form: DLL!Name."""
        return '%s!%s' % (self.dll, self.name)


class Dll:
    """A parsed PE32 DLL. `data` is the file as read; nothing is changed in place."""

    def __init__(self, data):
        self.data = bytes(data)
        self._parse()

    # -- reading --------------------------------------------------------------------------------

    def _u(self, fmt, offset):
        size = struct.calcsize(fmt)
        if offset < 0 or offset + size > len(self.data):
            raise PeError('truncated: %d bytes at file offset 0x%X are past the end' % (size, offset))
        return struct.unpack_from(fmt, self.data, offset)

    def _parse(self):
        if self.data[:2] != b'MZ':
            raise PeError('no MZ header')
        pe = self._u('<I', 0x3C)[0]
        if self.data[pe:pe + 4] != b'PE\0\0':
            raise PeError('no PE signature')
        machine, nsec, _, _, _, opt_size, chars = self._u('<HHIIIHH', pe + 4)
        if machine != IMAGE_FILE_MACHINE_I386:
            raise PeError('machine 0x%04X, not i386' % machine)
        if not chars & IMAGE_FILE_DLL:
            raise PeError('not a DLL')
        opt = pe + 24
        if self._u('<H', opt)[0] != PE32_MAGIC:
            raise PeError('not a PE32 optional header')
        self.entry_rva = self._u('<I', opt + 16)[0]
        self.image_base, self.section_alignment, self.file_alignment = self._u('<III', opt + 28)
        self.size_of_image, self.size_of_headers = self._u('<II', opt + 56)
        self._image_base_offset = opt + 28
        ndirs = self._u('<I', opt + 92)[0]
        self.dirs = [self._u('<II', opt + 96 + 8 * i) if i < ndirs else (0, 0) for i in range(16)]
        self.sections = []
        so = opt + opt_size
        for i in range(nsec):
            raw = self.data[so + 40 * i:so + 40 * i + 8]
            if len(raw) < 8:
                raise PeError('truncated section table')
            name = raw.rstrip(b'\0').decode('latin1')
            vsize, rva, rsize, roff = self._u('<IIII', so + 40 * i + 8)
            chars_s = self._u('<I', so + 40 * i + 36)[0]
            if rsize and roff + rsize > len(self.data):
                raise PeError('section %s runs past the end of the file' % name)
            self.sections.append(Section(name, rva, vsize, roff, rsize, chars_s))
        if self.size_of_headers > len(self.data):
            raise PeError('headers run past the end of the file')

    def offset(self, rva):
        """The file offset of `rva`, or None when no section's raw data holds it."""
        if rva < self.size_of_headers:
            return rva
        for s in self.sections:
            if s.rva <= rva < s.rva + min(s.size, s.raw_size):
                return s.raw_offset + rva - s.rva
        return None

    def read(self, rva, n):
        off = self.offset(rva)
        if off is None or off + n > len(self.data):
            raise PeError('rva 0x%X (+%d) is not in the file' % (rva, n))
        return self.data[off:off + n]

    def cstring(self, rva, limit=512):
        off = self.offset(rva)
        if off is None:
            raise PeError('string at rva 0x%X is not in the file' % rva)
        end = self.data.find(b'\0', off, off + limit)
        if end < 0:
            raise PeError('unterminated string at rva 0x%X' % rva)
        return self.data[off:end].decode('latin1')

    def section_of(self, rva):
        for s in self.sections:
            if s.rva <= rva < s.rva + s.size:
                return s
        return None

    # -- tables ---------------------------------------------------------------------------------

    def exports(self):
        """{name: rva} of the export table."""
        rva, size = self.dirs[0]
        if not rva:
            return {}
        offset = self.offset(rva)
        if offset is None:
            raise PeError('the export directory is not in the file')
        _, _, _, _, _, _, nfuncs, nnames, funcs, names, ords = self._u('<IIHHIIIIIII', offset)
        out = {}
        for i in range(nnames):
            name = self.cstring(struct.unpack('<I', self.read(names + 4 * i, 4))[0])
            ordinal = struct.unpack('<H', self.read(ords + 2 * i, 2))[0]
            if ordinal >= nfuncs:
                raise PeError('export %s names ordinal %d of %d' % (name, ordinal, nfuncs))
            target = struct.unpack('<I', self.read(funcs + 4 * ordinal, 4))[0]
            if rva <= target < rva + size:
                raise PeError('export %s is forwarded, which a mission never does' % name)
            out[name] = target
        return out

    def imports(self):
        """Every import in the import table's own order."""
        rva = self.dirs[1][0]
        out = []
        if not rva:
            return out
        p = rva
        while True:
            oft, _, _, name_rva, ft = struct.unpack('<IIIII', self.read(p, 20))
            if not name_rva and not ft:
                break
            dll = self.cstring(name_rva).upper()
            lookup = oft or ft
            i = 0
            while True:
                entry = struct.unpack('<I', self.read(lookup + 4 * i, 4))[0]
                if not entry:
                    break
                if entry & 0x80000000:
                    name = '#%d' % (entry & 0xFFFF)
                else:
                    name = self.cstring(entry + 2)
                out.append(Import(dll, name, ft + 4 * i))
                i += 1
            p += 20
        return out

    def relocations(self):
        """The rva of every HIGHLOW fixup, in table order. Any other type is refused."""
        rva, size = self.dirs[5]
        out = []
        p, end = rva, rva + size
        while p < end:
            page, block = struct.unpack('<II', self.read(p, 8))
            if block < 8:
                raise PeError('relocation block at rva 0x%X has size %d' % (p, block))
            for k in range((block - 8) // 2):
                word = struct.unpack('<H', self.read(p + 8 + 2 * k, 2))[0]
                kind, at = word >> 12, word & 0xFFF
                if kind == REL_ABSOLUTE:
                    continue
                if kind != REL_HIGHLOW:
                    raise PeError('relocation type %d at rva 0x%X' % (kind, page + at))
                out.append(page + at)
            p += block
        return out

    # -- rebasing -------------------------------------------------------------------------------

    def rebased(self, new_base):
        """The file's bytes with every HIGHLOW fixup and the header's ImageBase moved to `new_base`.

        Fixups are applied to the file's raw section data; a fixup whose field lies in no raw data
        (a zero-filled tail) would hold 0 + delta in the image, which a linker never emits, so it is
        refused."""
        delta = (new_base - self.image_base) & 0xFFFFFFFF
        out = bytearray(self.data)
        for rva in self.relocations():
            off = self.offset(rva)
            if off is None or off + 4 > len(out):
                raise PeError('relocation at rva 0x%X is outside the file' % rva)
            value = struct.unpack_from('<I', out, off)[0]
            struct.pack_into('<I', out, off, (value + delta) & 0xFFFFFFFF)
        struct.pack_into('<I', out, self._image_base_offset, new_base)
        return Dll(out)

    def code_pointer_seeds(self):
        """Addresses in the code sections that a fixup outside the code sections holds: function
        pointers in data (vtables, callback and exception tables). Fixups inside code are skipped,
        since they also hold jump-table targets in the middle of a function."""
        code = [s for s in self.sections if s.executable]
        seeds = set()
        for rva in self.relocations():
            sec = self.section_of(rva)
            if sec is None or sec.executable:
                continue
            target = struct.unpack('<I', self.read(rva, 4))[0] - self.image_base
            if any(s.rva <= target < s.rva + s.size for s in code):
                seeds.add(self.image_base + target)
        return sorted(seeds)

    def image_segments(self):
        """(rva, image size, bytes) for the headers and each section, as a loader maps them: the
        section's raw data, zero-filled to its virtual size."""
        segs = [(0, self.size_of_headers, self.data[:self.size_of_headers])]
        for s in self.sections:
            raw = self.data[s.raw_offset:s.raw_offset + min(s.raw_size, s.size)] if s.raw_size else b''
            segs.append((s.rva, s.size, raw))
        return segs


def sha256(data):
    return hashlib.sha256(data).hexdigest()
