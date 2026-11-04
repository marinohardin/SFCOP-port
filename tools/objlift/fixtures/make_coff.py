"""Write small i386 COFF objects for tests/test_objlift.py, laid out as VC6 lays out its own:
section headers, raw data, each section's relocations, the symbol table with a section-definition
record after every section symbol, and the string table.

    obj = Obj()
    text = obj.section('.text', code, CODE | COMDAT, selection=ANY)
    obj.define('_f', text, 0, function=True)
    obj.reloc(text, 1, '_g', REL32)          # '_g' becomes an undefined external
    open(path, 'wb').write(obj.build())

Every value is written out by the test that uses it; nothing here comes from a compiler.
"""
import struct

CODE = 0x60000020           # code, execute, read
RDATA = 0x40000040          # initialised data, read
DATA = 0xC0000040           # initialised data, read, write
BSS = 0xC0000080            # uninitialised data, read, write
COMDAT = 0x00001000
INFO = 0x00000A00           # .drectve: LNK_INFO | LNK_REMOVE
ALIGN_1, ALIGN_4, ALIGN_16 = 0x00100000, 0x00300000, 0x00500000

NODUPLICATES, ANY, SAME_SIZE, EXACT_MATCH, ASSOCIATIVE, LARGEST = 1, 2, 3, 4, 5, 6

ABSOLUTE, DIR32, DIR32NB, SECTION, SECREL, REL32 = 0x00, 0x06, 0x07, 0x0A, 0x0B, 0x14

EXTERNAL, STATIC = 2, 3
FUNCTION_TYPE = 0x20


class _Section:
    def __init__(self, number, name, data, chars, selection, associated):
        self.number, self.name, self.data, self.chars = number, name, bytes(data), chars
        self.selection, self.associated = selection, associated
        self.relocs = []        # (offset, symbol name, type)
        self.symbols = []       # (name, value, storage, type)


class Obj:
    def __init__(self):
        self.sections = []
        self.undefined = []     # names referenced and defined nowhere in this object
        self.absolute = []      # (name, value)

    def section(self, name, data, chars, selection=0, associated=0):
        sec = _Section(len(self.sections) + 1, name, data, chars, selection, associated)
        self.sections.append(sec)
        return sec

    def define(self, name, sec, value, function=False, static=False):
        sec.symbols.append((name, value, STATIC if static else EXTERNAL,
                            FUNCTION_TYPE if function else 0))

    def define_absolute(self, name, value):
        self.absolute.append((name, value))

    def reloc(self, sec, offset, name, typ):
        sec.relocs.append((offset, name, typ))

    # -- writing -----------------------------------------------------------------------------------

    def build(self):
        strings = bytearray(b'\0\0\0\0')

        def name8(name):
            raw = name.encode('latin1')
            if len(raw) <= 8:
                return raw.ljust(8, b'\0')
            off = len(strings)
            strings.extend(raw + b'\0')
            return b'\0\0\0\0' + struct.pack('<I', off)

        # The symbol table: per section its definition (with one aux record), then its symbols;
        # then absolute symbols, then undefined externals.
        records, index = [], {}

        def add(name, value, secnum, typ, storage, aux=None):
            index.setdefault(name, len(records))
            records.append(struct.pack('<8sIhHBB', name8(name), value, secnum, typ, storage,
                                       1 if aux else 0))
            if aux:
                records.append(aux)

        for sec in self.sections:
            aux = struct.pack('<IHHIHB3x', len(sec.data), len(sec.relocs), 0, 0,
                              sec.associated, sec.selection)
            add(sec.name, 0, sec.number, 0, STATIC, aux)
            for name, value, storage, typ in sec.symbols:
                add(name, value, sec.number, typ, storage)
        for name, value in self.absolute:
            add(name, value, -1, 0, STATIC)
        defined = set(index)
        for sec in self.sections:
            for _, name, _ in sec.relocs:
                if name not in defined and name not in self.undefined:
                    self.undefined.append(name)
        for name in self.undefined:
            add(name, 0, 0, FUNCTION_TYPE, EXTERNAL)

        # Layout: header, section headers, then each section's data and relocations.
        at = 20 + 40 * len(self.sections)
        headers, body = [], bytearray()
        for sec in self.sections:
            bss = sec.chars & 0x80
            dataoff = 0 if bss else at + len(body)
            if not bss:
                body.extend(sec.data)
            reloff = at + len(body) if sec.relocs else 0
            for off, name, typ in sec.relocs:
                body.extend(struct.pack('<IIH', off, index[name], typ))
            headers.append(struct.pack('<8sIIIIIIHHI', name8(sec.name), 0, 0, len(sec.data),
                                       dataoff, reloff, 0, len(sec.relocs), 0, sec.chars))
        symoff = at + len(body)
        header = struct.pack('<HHIIIHH', 0x14C, len(self.sections), 0, symoff, len(records), 0, 0)
        strings[0:4] = struct.pack('<I', len(strings))
        return header + b''.join(headers) + bytes(body) + b''.join(records) + bytes(strings)
