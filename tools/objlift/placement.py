"""Place the sections of VC6 COFF objects in the guest and resolve their relocations (stdlib only).

The objects are read with the harness's COFF reader (harness/lib/coff.py). This module adds what
a linker needs on top of it: the section-definition records of COMDAT sections, which sections are
live, where each one goes, and the final value of every relocated field.

Where a section goes, in order of preference:

- **claimed**: the section holds a function the manifest claims. The section is placed so that the
  function sits at its original address in the exe. Its code is lifted under the exe lift's name
  for that address (`sub_006FC5A0`) and replaces the lifted-exe body. Its bytes are not loaded:
  lifted code does not execute from guest memory, and the exe's own bytes stay where they are.
- **exe**: the exe already holds the section's contents at a known address: one of its symbols has
  an address in the exe's symbol evidence (`ghidra/exports/functions.csv`, the SDK matches), or
  the original bytes of an exact claimed function use one consistent address for it (the
  harness's relocation check, harness/lib/target.py). Nothing is lifted or loaded; references go to
  the exe's copy, which is the one the original link chose.
- **fresh**: anything else, at the next free address in the reserved guest range. Its code is
  lifted under `sub_<address>`; its bytes go into the data image that the host loads at startup.
  A writable section is refused here: the lifted exe would keep its own copy of that storage and
  the two would drift apart.

Only live sections are placed: those holding a claimed function, and those a placed section
references, the way a linker with /OPT:REF keeps sections (COMDAT and plain alike). An
associative COMDAT section lives and dies with its parent. A section the exe holds ends the walk:
what it points at is the exe's business.

A data section whose symbols the exe holds at addresses that do not keep the object's layout (the
original link put each variable apart) is held "symbol by symbol": each reference must then name a
symbol with an exe address.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
if REPO not in sys.path:
    sys.path.insert(0, REPO)

from harness.lib import coff  # noqa: E402

# The reserved guest range for fresh sections. The exe's image ends at 0x00C86000 and pcrecomp's
# guest memory hands out "anywhere" reservations from 0x10000000 up (runtime/win32hle/guest_mem.c,
# ANY_LO), so nothing else in the lifted game maps these 16 MB.
RESERVED_BASE = 0x0F000000
RESERVED_SIZE = 0x01000000
IMAGE_BASE = 0x00400000

# Section characteristics
SCN_CNT_CODE = 0x00000020
SCN_CNT_UNINITIALIZED = 0x00000080
SCN_LNK_INFO = 0x00000200
SCN_LNK_REMOVE = 0x00000800
SCN_LNK_COMDAT = 0x00001000
SCN_ALIGN_MASK = 0x00F00000
SCN_MEM_WRITE = 0x80000000

# COMDAT selection (the section-definition auxiliary record)
SELECT_NODUPLICATES = 1
SELECT_ANY = 2
SELECT_SAME_SIZE = 3
SELECT_EXACT_MATCH = 4
SELECT_ASSOCIATIVE = 5
SELECT_LARGEST = 6
SELECTION_NAMES = {1: 'nodupes', 2: 'any', 3: 'same size', 4: 'exact match', 5: 'associative',
                   6: 'largest'}

REL_ABSOLUTE = 0x00
SUPPORTED_RELOCS = (coff.REL_DIR32, coff.REL_DIR32NB, coff.REL_SECTION, coff.REL_SECREL,
                    coff.REL_REL32)


class PlacementError(Exception):
    """An input the lift cannot honour: the message names the symbol or section."""


class Claim:
    """A function the manifest claims: its original address and size, its compiled symbol, the
    unit."""
    __slots__ = ('address', 'size', 'symbol', 'name', 'source')

    def __init__(self, address, size, symbol, name, source):
        self.address, self.size, self.symbol = address, size, symbol
        self.name, self.source = name, source


class InputSection:
    """One section of one input object."""
    __slots__ = ('obj', 'number', 'name', 'data', 'relocs', 'characteristics', 'selection',
                 'associated', 'comdat_symbol', 'checksum', 'symbols', 'mode', 'va', 'claims',
                 'evidence', 'split')

    def __init__(self, obj, sec):
        self.obj, self.number, self.name = obj, sec.number, sec.name
        self.data, self.relocs, self.characteristics = sec.data, sec.relocs, sec.characteristics
        self.selection = 0
        self.associated = 0
        self.checksum = 0
        self.comdat_symbol = None
        self.symbols = []           # Symbols defined in this section
        self.mode = None            # 'claimed', 'exe', 'fresh'
        self.va = None
        self.claims = []            # Claims whose function is in this section
        self.evidence = ''          # why it is where it is
        self.split = None           # exe mode, symbol by symbol: {symbol name: exe address}

    @property
    def label(self):
        return f'{os.path.basename(self.obj.path)}:{self.name}#{self.number}'

    @property
    def is_code(self):
        return bool(self.characteristics & SCN_CNT_CODE)

    @property
    def is_writable(self):
        return bool(self.characteristics & SCN_MEM_WRITE)

    @property
    def is_bss(self):
        return bool(self.characteristics & SCN_CNT_UNINITIALIZED)

    @property
    def is_comdat(self):
        return bool(self.characteristics & SCN_LNK_COMDAT)

    @property
    def alignment(self):
        n = (self.characteristics & SCN_ALIGN_MASK) >> 20
        return 1 << (n - 1) if n else 16

    @property
    def dropped(self):
        """Sections a linker never puts in the image: directives, removable data, debug info."""
        return bool(self.characteristics & (SCN_LNK_INFO | SCN_LNK_REMOVE)) \
            or self.name.startswith('.debug')


class InputObject:
    """A COFF object with its section-definition records read."""

    def __init__(self, path, unit=None):
        self.path, self.unit = path, unit
        raw = open(path, 'rb').read()
        try:
            self.coff = coff.CoffObject(raw)
        except (coff.CoffError, struct.error) as e:
            raise PlacementError(f'{path}: {e}') from None
        self.sections = {n: InputSection(self, s) for n, s in self.coff.sections.items()}
        symoff = struct.unpack_from('<I', raw, 8)[0]
        for sym in self.coff.symbols.values():
            sec = self.sections.get(sym.section)
            if sec is None:
                continue
            if sym.storage == coff.SYM_CLASS_STATIC and sym.naux and sym.value == 0 \
                    and sym.name == sec.name:
                # Section definition: length, relocs, lines, checksum, number, selection.
                aux = raw[symoff + (sym.index + 1) * 18:symoff + (sym.index + 2) * 18]
                _, _, _, checksum, number, selection = struct.unpack_from('<IHHIHB', aux)
                if sec.is_comdat:
                    sec.selection, sec.associated, sec.checksum = selection, number, checksum
                continue
            sec.symbols.append(sym)
            if sec.is_comdat and sec.comdat_symbol is None and sec.selection != SELECT_ASSOCIATIVE:
                sec.comdat_symbol = sym   # the first symbol after the section's own is its key
        for sec in self.sections.values():
            for r_off, symidx, typ in sec.relocs:
                if symidx not in self.coff.symbols:
                    raise PlacementError(f'{sec.label}: relocation at +0x{r_off:x} names symbol '
                                         f'index {symidx}, which does not exist')

    def symbol(self, index):
        return self.coff.symbols[index]


class Resolved:
    """Where a symbol ends up: an address and how it was found."""
    __slots__ = ('address', 'how')

    def __init__(self, address, how):
        self.address, self.how = address, how


class Placement:
    """The result: every live section placed and every relocated field computed."""

    def __init__(self):
        self.sections = []          # live InputSections, in placement order
        self.dropped = []           # (section label, why)
        self.relocations = []       # (section label, offset, type name, symbol, address, how)
        self.notes = []             # judgement calls made on the way, one sentence each
        self.reserved_end = RESERVED_BASE
        self.images = {}            # InputSection -> relocated bytes

    def code_sections(self):
        return [s for s in self.sections if s.is_code and s.mode in ('claimed', 'fresh')]

    def fresh_sections(self):
        return [s for s in self.sections if s.mode == 'fresh']


class SymbolIndex:
    """Names the exe answers for: decorated name -> set of addresses (harness/lib/funcs.py's
    FunctionIndex.known), plus which of them are code."""

    def __init__(self, known=None, code=None, harness_index=None):
        self.known = known or {}
        self.code = code or set()
        self.harness_index = harness_index     # for the relocation check of exact functions

    @classmethod
    def from_harness(cls, functions_csv, symbol_tsvs):
        from harness.lib import funcs
        index = funcs.FunctionIndex(functions_csv, symbol_tsvs)
        known = {k: set(v) for k, v in index.known.items() if k != '__except_list'}
        return cls(known, set(index.by_address), index)


def _align(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def _find_definitions(objects):
    """External name -> the InputSection and Symbol that define it, after COMDAT selection."""
    defs = {}
    for obj in objects:
        for sec in obj.sections.values():
            if sec.dropped:
                continue
            for sym in sec.symbols:
                if sym.storage != coff.SYM_CLASS_EXTERNAL:
                    continue
                prev = defs.get(sym.name)
                if prev is None:
                    defs[sym.name] = (sec, sym)
                    continue
                psec, _ = prev
                defs[sym.name] = _select(psec, sec, sym, prev)
    return defs


def _select(psec, sec, sym, prev):
    """COMDAT selection between two definitions of one name."""
    if not (psec.is_comdat and sec.is_comdat) or sec.comdat_symbol is not sym:
        raise PlacementError(f'{sym.name} is defined twice ({psec.label} and {sec.label})')
    if psec.selection != sec.selection:
        raise PlacementError(f'{sym.name}: COMDAT copies disagree on the selection kind '
                             f'({psec.label} has {psec.selection}, {sec.label} has {sec.selection})')
    sel = psec.selection
    if sel == SELECT_NODUPLICATES or sec.selection == SELECT_NODUPLICATES:
        raise PlacementError(f'{sym.name}: COMDAT with selection "no duplicates" defined in '
                             f'{psec.label} and {sec.label}')
    if sel == SELECT_SAME_SIZE and len(psec.data) != len(sec.data):
        raise PlacementError(f'{sym.name}: COMDAT "same size" differs ({len(psec.data)} and '
                             f'{len(sec.data)} bytes)')
    if sel == SELECT_EXACT_MATCH and (psec.data != sec.data or psec.checksum != sec.checksum):
        raise PlacementError(f'{sym.name}: COMDAT "exact match" differs between {psec.label} '
                             f'and {sec.label}')
    if sel == SELECT_LARGEST and len(sec.data) > len(psec.data):
        return sec, sym
    return prev


def _claimed_sections(objects, claims):
    """Attach each claim to the section that defines its symbol; place that section."""
    by_symbol = {}
    for obj in objects:
        for sec in obj.sections.values():
            for sym in sec.symbols:
                if sym.storage == coff.SYM_CLASS_EXTERNAL and not sec.dropped:
                    by_symbol.setdefault(sym.name, []).append((sec, sym))
    for c in claims:
        hits = by_symbol.get(c.symbol)
        if not hits:
            raise PlacementError(f'0x{c.address:08x} {c.name}: its symbol {c.symbol} is not '
                                 f'defined in any input object')
        sec, sym = hits[0]
        if not sec.is_code:
            raise PlacementError(f'0x{c.address:08x} {c.symbol} is not in a code section '
                                 f'({sec.label})')
        base = (c.address - sym.value) & 0xFFFFFFFF
        if sec.va is not None and sec.va != base:
            raise PlacementError(f'{sec.label} holds claimed functions at inconsistent addresses: '
                                 f'0x{sec.va:08x} and 0x{base:08x} for its start (compile the unit '
                                 f'with /Gy so each function has its own section)')
        sec.mode, sec.va = 'claimed', base
        sec.claims.append(c)
        sec.evidence = 'claimed in the manifest'


class Placer:
    def __init__(self, objects, claims, index, evidence=None, exe_sections=None,
                 reserved=(RESERVED_BASE, RESERVED_SIZE)):
        self.objects, self.claims, self.index = objects, claims, index
        # (object path or None for an external name, symbol name) -> exe address, from the
        # original bytes of exact claimed functions (exact_function_evidence)
        self.evidence = evidence or {}
        # [(start, end)] of the exe's sections, for SECTION and SECREL relocations
        self.exe_sections = exe_sections
        self.base, self.size = reserved
        self.result = Placement()
        self.defs = _find_definitions(objects)

    # -- liveness -------------------------------------------------------------------------------

    def _target_section(self, obj, sym):
        """The input section a reference to `sym` (from obj) lands in, or None for the exe."""
        if sym.section > 0:
            sec = obj.sections[sym.section]
            if sym.storage == coff.SYM_CLASS_EXTERNAL and sym.name in self.defs:
                return self.defs[sym.name][0]
            return sec
        if sym.section == 0 and sym.name in self.defs:
            return self.defs[sym.name][0]
        return None

    def _record_dropped(self, seen):
        for obj in self.objects:
            for s in obj.sections.values():
                if id(s) in seen:
                    continue
                why = 'never put in an image' if s.dropped else 'not referenced by claimed code'
                if s.is_comdat and s.comdat_symbol is not None and \
                        self.defs.get(s.comdat_symbol.name, (s,))[0] is not s:
                    why = f'COMDAT duplicate ({SELECTION_NAMES.get(s.selection, s.selection)})'
                self.result.dropped.append((s.label, why))

    # -- where each live section goes --------------------------------------------------------------

    def _exe_address(self, name):
        """The exe's address for a decorated name, or None. Several addresses: code picks the
        lowest (the duplicates are copies of one function), data is refused."""
        addrs = self.index.known.get(name)
        if not addrs:
            return None
        if len(addrs) == 1:
            return next(iter(addrs))
        if all(a in self.index.code for a in addrs):
            low = min(addrs)
            self.result.notes.append(f'{name} has {len(addrs)} addresses in the exe ('
                                     + ', '.join(f'0x{a:08x}' for a in sorted(addrs))
                                     + f'), copies of one function; 0x{low:08x} is used')
            return low
        raise PlacementError(f'{name} names {len(addrs)} addresses in the exe ('
                             + ', '.join(f'0x{a:08x}' for a in sorted(addrs)) + '); a data '
                             'symbol must have one')

    def _exe_addresses(self, sec):
        """[(symbol, exe address, evidence)] for the section's symbols the exe has an address for."""
        found = []
        for sym in sec.symbols:
            external = sym.storage == coff.SYM_CLASS_EXTERNAL
            addr = self._exe_address(sym.name) if external else None
            how = 'exe symbol'
            seen = self.evidence.get((None if external else sec.obj.path, sym.name))
            if addr is None and seen is not None:
                addr, how = seen, 'used by an exact function'
            if addr is not None:
                found.append((sym, addr, f'{how} {sym.name}'))
        return found

    def _decide(self, sec):
        """exe (whole, or symbol by symbol for data the original link laid out differently),
        or fresh."""
        found = self._exe_addresses(sec)
        if found:
            starts = {(addr - sym.value) & 0xFFFFFFFF for sym, addr, _ in found}
            if len(starts) == 1:
                sec.mode, sec.va, sec.evidence = 'exe', starts.pop(), found[0][2]
                return
            if sec.is_code:
                raise PlacementError(f'{sec.label}: its symbols put the code at different '
                                     'addresses in the exe (' + '; '.join(
                                         f'0x{a:08x} by {w}' for _, a, w in found) + ')')
            # The original link put these variables apart (each a symbol of its own there):
            # every reference must then name one of them.
            sec.mode, sec.split = 'exe', {sym.name: addr for sym, addr, _ in found}
            sec.evidence = f'the exe holds its {len(found)} symbols apart'
            return
        if sec.is_writable:
            names = ', '.join(s.name for s in sec.symbols) or sec.name
            raise PlacementError(
                f'{sec.label} is writable and has no address in the exe ({names}): placed '
                'fresh, the lifted exe would keep its own copy of it. Give one of its symbols '
                'exe evidence, or make the function that uses it exact')
        sec.mode = 'fresh'

    def place(self):
        """Walk from the claimed sections through their relocations. A section the exe holds ends
        the walk there: its contents, and what they point at, are the exe's."""
        live, seen = [], set()
        work = [s for o in self.objects for s in o.sections.values() if s.mode == 'claimed']
        while work:
            sec = work.pop(0)
            if id(sec) in seen:
                continue
            seen.add(id(sec))
            live.append(sec)
            if sec.mode is None:
                self._decide(sec)
            if sec.mode == 'exe':
                continue
            for _, symidx, typ in sec.relocs:
                if typ == REL_ABSOLUTE:
                    continue
                tgt = self._target_section(sec.obj, sec.obj.symbol(symidx))
                if tgt is not None and not tgt.dropped:
                    work.append(tgt)
            for other in sec.obj.sections.values():
                if other.selection == SELECT_ASSOCIATIVE and other.associated == sec.number \
                        and not other.dropped:
                    work.append(other)
        self._record_dropped(seen)
        fresh = [s for s in live if s.mode == 'fresh']
        # Fresh sections: code first, then read-only data, in input order.
        at = self.base
        for sec in sorted(fresh, key=lambda s: (not s.is_code, s.is_bss)):
            at = _align(at, sec.alignment)
            sec.mode, sec.va = 'fresh', at
            sec.evidence = 'reserved guest range'
            at += len(sec.data)
        if at > self.base + self.size:
            raise PlacementError(f'fresh sections need 0x{at - self.base:x} bytes; the reserved '
                                 f'guest range holds 0x{self.size:x}')
        self.result.reserved_end = at
        self.result.sections = [s for s in live]
        self._relocate()
        return self.result

    # -- relocation --------------------------------------------------------------------------------

    def resolve(self, sec, sym):
        if sym.section == -1:                                   # IMAGE_SYM_ABSOLUTE
            return Resolved(sym.value, 'absolute')
        tgt = self._target_section(sec.obj, sym)
        if tgt is not None:
            if tgt.va is None and tgt.split is None:
                raise PlacementError(f'{sym.name}: its section {tgt.label} was not placed')
            if sym.section > 0 and tgt is sec.obj.sections.get(sym.section):
                value = sym.value
            else:                                               # COMDAT chosen in another object
                value = self.defs[sym.name][1].value
            if tgt.split is not None:
                if sym.name not in tgt.split:
                    raise PlacementError(f'{sym.name} (referenced from {sec.label}): the exe holds '
                                         f'{tgt.label} symbol by symbol, and has no address for '
                                         'this one')
                return Resolved(tgt.split[sym.name], f'exe, apart, {tgt.label}')
            return Resolved((tgt.va + value) & 0xFFFFFFFF, f'{tgt.mode} {tgt.label}')
        if sym.section == 0:
            addr = self._exe_address(sym.name)
            if addr is not None:
                return Resolved(addr, 'exe symbol')
            if (None, sym.name) in self.evidence:
                return Resolved(self.evidence[(None, sym.name)], 'used by an exact function')
            raise PlacementError(f'unresolved external symbol {sym.name} (referenced from '
                                 f'{sec.label}): it is neither in the input objects nor in the '
                                 'exe\'s symbol evidence')
        raise PlacementError(f'{sym.name}: section number {sym.section} is not one this tool '
                             'handles')

    def _relocate(self):
        for sec in self.result.sections:
            if sec.mode == 'exe':
                continue
            data = bytearray(sec.data)
            for r_off, symidx, typ in sorted(sec.relocs):
                sym = sec.obj.symbol(symidx)
                if typ == REL_ABSOLUTE:
                    continue
                if typ not in SUPPORTED_RELOCS:
                    raise PlacementError(f'{sec.label}+0x{r_off:x}: relocation type 0x{typ:x} '
                                         f'against {sym.name} is not one VC6 emits for x86 code')
                width = 2 if typ == coff.REL_SECTION else 4
                if r_off + width > len(data):
                    raise PlacementError(f'{sec.label}+0x{r_off:x}: relocation past the end of '
                                         'the section')
                res = self.resolve(sec, sym)
                p = (sec.va + r_off) & 0xFFFFFFFF
                s = res.address
                if typ == coff.REL_SECTION:
                    field = struct.unpack_from('<H', data, r_off)[0]
                    struct.pack_into('<H', data, r_off, (field + self.section_index(s, sym)) & 0xFFFF)
                else:
                    field = struct.unpack_from('<I', data, r_off)[0]
                    if typ == coff.REL_DIR32:
                        value = field + s
                    elif typ == coff.REL_REL32:
                        value = field + s - (p + 4)
                    elif typ == coff.REL_DIR32NB:
                        value = field + s - IMAGE_BASE
                    else:                                       # SECREL
                        value = field + s - self.section_start(s, sym)
                    struct.pack_into('<I', data, r_off, value & 0xFFFFFFFF)
                self.result.relocations.append(
                    (sec.label, r_off, coff.REL_NAMES[typ], sym.name, s, res.how))
            self.result.images[sec] = bytes(data)

    # The output sections SECTION and SECREL refer to: the exe's own for exe addresses, and one
    # more, numbered after them, for the reserved range.
    def _output_section(self, address, sym):
        if self.exe_sections is None:
            raise PlacementError(f'a SECTION or SECREL relocation against {sym.name} needs the '
                                 'exe\'s section table (give --exe)')
        for i, (lo, hi) in enumerate(self.exe_sections):
            if lo <= address < hi:
                return i + 1, lo
        if self.base <= address < self.base + self.size:
            return len(self.exe_sections) + 1, self.base
        raise PlacementError(f'{sym.name} at 0x{address:08x} is in no section of the image')

    def section_index(self, address, sym):
        return self._output_section(address, sym)[0]

    def section_start(self, address, sym):
        return self._output_section(address, sym)[1]


def exact_function_evidence(claims, objects, exe, index):
    """Addresses the original bytes of exact claimed functions use for symbols the exe evidence
    does not name: {symbol: address}. Uses the harness's own relocation check
    (harness/lib/target.py): only a function whose masked bytes match the original exactly, and
    only relocations it calls consistent ('ok' or 'unverified'), count. Returns (evidence, notes)."""
    from harness.lib import target
    out, notes = {}, []
    by_symbol = {}
    for obj in objects:
        for name, sym in obj.coff.by_name.items():
            if sym.section > 0:
                by_symbol.setdefault(name, obj)
    for c in claims:
        obj = by_symbol.get(c.symbol)
        if obj is None:
            continue
        t = target.build(c.address, c.size, obj.coff, c.symbol, exe, index)
        if not t['strict']:
            notes.append(f'0x{c.address:08x} {c.symbol} is not byte-exact against the exe: its '
                         'relocations give no evidence for addresses')
            continue
        for r in t['relocs']:
            if r.exe_address is None or r.status not in ('ok', 'unverified') \
                    or r.detail in ('own section', 'absolute'):
                continue
            sym = obj.coff.by_name[r.symbol]
            key = (None if sym.storage == coff.SYM_CLASS_EXTERNAL else obj.path, r.symbol)
            prev = out.get(key)
            if prev is not None and prev != r.exe_address:
                raise PlacementError(f'{r.symbol}: exact functions use it at 0x{prev:08x} and '
                                     f'0x{r.exe_address:08x}')
            out[key] = r.exe_address
    return out, notes
