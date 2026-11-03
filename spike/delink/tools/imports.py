#!/usr/bin/env python3
"""Map every import slot of the exe to the symbol an import library defines for it.

For each DLL the exe imports from, the VC6 import library of that name is searched for the
short import entry that resolves to the same name or ordinal; its public symbol gives the
__imp_ name the delinked object must reference (e.g. __imp__GetProcAddress@8). DLLs with no
VC6 library (DBGHELP, mss32) get a .def file, built into a library by relink.sh, and the names
it will produce.

Usage: imports.py <exe> <vc6 lib dir> <out dir>
Writes <out>/imports.tsv (iat_va, dll, import, symbol, library) and <out>/<dll>.def.
"""
import os
import sys

import pecoff

# DLLs whose import library is generated here; everything else comes from the VC6 Lib set.
GENERATED = {'DBGHELP.dll', 'mss32.dll'}


def main(exe, libdir, outdir):
    pe = pecoff.PE.load(exe)
    os.makedirs(outdir, exist_ok=True)
    libs = {f.upper(): os.path.join(libdir, f) for f in os.listdir(libdir)}
    rows = []
    for dll, entries in pe.imports():
        stem = dll.rsplit('.', 1)[0]
        if dll in GENERATED:
            lines = ['LIBRARY %s' % dll, 'EXPORTS']
            for name, ordinal, slot in entries:
                assert name is not None, 'ordinal import from %s' % dll
                # LIB strips one leading underscore from the import name (name type NOPREFIX)
                # and adds one to the symbol, so the import name stays exactly `name`.
                lines.append('  %s' % name)
                rows.append((slot, dll, name, '__imp__' + name, stem + '.lib'))
            with open(os.path.join(outdir, stem + '.def'), 'w') as f:
                f.write('\n'.join(lines) + '\n')
            continue
        lib = libs.get(stem.upper() + '.LIB')
        if lib is None:
            sys.exit('no import library for %s' % dll)
        with open(lib, 'rb') as f:
            table = pecoff.short_imports(f.read())
        by_name = {e['imp']: e for e in table if e['name_type'] != 'ordinal' and e['kind'] == 0}
        by_ord = {e['imp']: e for e in table if e['name_type'] == 'ordinal' and e['kind'] == 0}
        for name, ordinal, slot in entries:
            e = by_name.get(name) if name is not None else by_ord.get(ordinal)
            if e is None:
                sys.exit('%s: %s not in %s' % (dll, name or '#%d' % ordinal, lib))
            rows.append((slot, dll, name or '#%d' % ordinal, '__imp_' + e['symbol'],
                         os.path.basename(lib)))
    rows.sort()
    with open(os.path.join(outdir, 'imports.tsv'), 'w') as f:
        f.write('iat_va\tdll\timport\tsymbol\tlibrary\n')
        for slot, dll, imp, sym, lib in rows:
            f.write('%08x\t%s\t%s\t%s\t%s\n' % (pe.image_base + slot, dll, imp, sym, lib))
    print('%d imports from %d DLLs -> %s' % (len(rows), len(pe.imports()), outdir))


if __name__ == '__main__':
    main(*sys.argv[1:])
