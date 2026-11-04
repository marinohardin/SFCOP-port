#!/usr/bin/env python3
"""Generate the import allow-list for native mission modules (DESIGN §6.4).

    gen_allowlist.py [--build-dir DIR] [--out FILE] [--jobs N]
    gen_allowlist.py --check IMPORTS [--allowlist FILE] [--module NAME]

A mission module may import only what the scripting library and the C runtime provide. This script
derives that set from the install at build time and writes it, sorted, one symbol per line, to
src/app/scripts/allowlist.txt (format and rule: src/app/scripts/ModuleContract.md). Nothing from
the SDK is copied into the repository; the list of names is what is committed.

The list is the union of four parts:

  library   every external symbol a member of the SDK's ScriptInterfaceR.lib defines;
  linked    the symbols that the Single template's objects and the ScriptInterfaceR.lib members
            reference and that the template's link resolved in another SDK library (SharedR,
            sfcTiger, dmp3D and its shim, ...);
  runtime   the same references resolved in the VC6 C and C++ runtime (LIBCMT, LIBCPMT): by the
            template's link, or, for a library member the template did not link, by the runtime
            libraries of the VC6 Lib set themselves;
  imports   the template .scr's PE import table, as `DLL!Name`.

Compiler-generated names that are never imported (string literals, floating-point constants,
dynamic initializers) are left out. The template is the SDK's Single-mission template, built with
toolchain/build_scr.sh (VC6 SP4 in the sfcop-msvc6 container); --build-dir reuses a build that
already exists (the staging directory build_scr.py leaves: <name>.map, <name>.scr, obj/*.obj).

--check reads an import list (one symbol per line, '#' comments) and refuses it, naming each
symbol that is not on the allow-list, with the message the native loader gives. It exits 0 when
every import is allowed and 1 otherwise.
"""
import argparse
import hashlib
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.dont_write_bytecode = True
sys.path.insert(0, REPO)
from harness.lib import paths  # noqa: E402  (finds the install from the main checkout or a worktree)

ALLOWLIST = os.path.join(REPO, 'src', 'app', 'scripts', 'allowlist.txt')
# The SDK sits beside the original exe in the install.
SDK_DIRNAME = 'OP API - R2.1'
HASHES = os.path.join(REPO, 'analysis', 'results', 'retail_hashes.sha256')
TEMPLATE = 'Single'
TEMPLATE_NAME = 'AllowListTemplate'
GENERATOR = 'tools/scr2native/gen_allowlist.py'

# Libraries by role, lower case, as the link map names them.
SDK_LIBS = {'scriptinterfacer', 'sharedr', 'sfctiger_msvcr', 'dmp3d_msvcr', 'bugslayerutil', 'cryptlibr'}
RUNTIME_LIBS = {'libcmt', 'libcpmt'}
# build_scr.py compiles this stand-in for the members missing from the truncated dmp3D_MSVCR.lib;
# its symbols belong to the dmp3D library.
SHIM_OBJ = '_dmp3d_shim.obj'

# The VC6 runtime libraries, in the Lib set toolchain/bin/msvc-run links with.
RUNTIME_LIB_FILES = ('LIBCMT.LIB', 'LIBCPMT.LIB')

# Compiler-generated names that no module imports: string literals, floating-point constants,
# dynamic initializers and their atexit partners, the throw descriptors every thrower emits for
# itself, and the import library's terminator.
_NOT_BINDABLE = re.compile(r'^(\?\?_C@|__real@|__xmm@|_?\$E\d|\?\?__[EF]|__CTA?\d?\?|__TI\d\?'
                           r'|__NULL_IMPORT_DESCRIPTOR$)')

REFUSAL = "refused: {module}: imports {symbol}, which is outside the scripting library and the C runtime"


def sdk_dir():
    return os.path.join(os.path.dirname(paths.EXE), SDK_DIRNAME)


# --- readers ------------------------------------------------------------------------------------

def ar_members(path):
    """(name, body) for each member of a COFF archive (.lib), the linker members skipped."""
    data = open(path, 'rb').read()
    if data[:8] != b'!<arch>\n':
        raise ValueError(f'{os.path.basename(path)} is not an archive')
    off, longnames, out = 8, b'', []
    while off + 60 <= len(data):
        hdr = data[off:off + 60]
        name = hdr[:16].decode('latin1').rstrip()
        size = int(hdr[48:58])
        body = data[off + 60:off + 60 + size]
        if name == '//':
            longnames = body
        elif name != '/':
            if name.startswith('/') and name[1:].isdigit():
                i = int(name[1:])
                name = longnames[i:longnames.index(b'\0', i)].decode('latin1')
            out.append((name.rstrip('/'), body))
        off += 60 + size + (size & 1)
    return out


def coff_externals(body):
    """(defined, undefined) external symbol names of an i386 COFF object; empty for anything else."""
    if len(body) < 20 or body[:2] != b'\x4c\x01':
        return set(), set()
    symptr, nsyms = struct.unpack_from('<II', body, 8)
    strtab = symptr + 18 * nsyms
    defined, undefined = set(), set()
    k = 0
    while k < nsyms:
        rec = symptr + 18 * k
        raw, value, section, _, storage, naux = struct.unpack_from('<8sIhHBB', body, rec)
        if raw[:4] == b'\0\0\0\0':
            o = strtab + struct.unpack_from('<I', raw, 4)[0]
            name = body[o:body.index(b'\0', o)].decode('latin1')
        else:
            name = raw.rstrip(b'\0').decode('latin1')
        if storage == 2:  # IMAGE_SYM_CLASS_EXTERNAL
            if section > 0 or (section == 0 and value != 0):  # value != 0 with no section: common
                defined.add(name)
            elif section == 0:
                undefined.add(name)
        elif storage == 105:  # IMAGE_SYM_CLASS_WEAK_EXTERNAL
            undefined.add(name)
        k += 1 + naux
    return defined, undefined


def pe_imports(path):
    """The import table of a PE file as 'DLL!Name' (by ordinal: 'DLL!#n'), DLL names upper case."""
    b = open(path, 'rb').read()
    pe = struct.unpack_from('<I', b, 0x3c)[0]
    nsec, optsz = struct.unpack_from('<H', b, pe + 6)[0], struct.unpack_from('<H', b, pe + 20)[0]
    opt = pe + 24
    irva = struct.unpack_from('<I', b, opt + 104)[0]
    secs = [struct.unpack_from('<8sIIII', b, opt + optsz + k * 40) for k in range(nsec)]

    def off(rva):
        for _, vsz, va, rsz, rptr in secs:
            if va <= rva < va + max(vsz, rsz):
                return rva - va + rptr
        raise ValueError(f'rva {rva:#x} outside sections')

    def cstr(o):
        return b[o:b.index(b'\0', o)].decode('latin1')

    out = []
    d = off(irva) if irva else None
    while d is not None:
        ilt, _, _, name_rva, iat = struct.unpack_from('<IIIII', b, d)
        if not name_rva:
            break
        dll = cstr(off(name_rva)).upper()
        t = off(ilt or iat)
        while True:
            entry = struct.unpack_from('<I', b, t)[0]
            if not entry:
                break
            out.append(f'{dll}!#{entry & 0xFFFF}' if entry & 0x80000000 else f'{dll}!{cstr(off(entry) + 2)}')
            t += 4
        d += 20
    return out


def map_publics(path):
    """Public symbol -> where the link took it from ('lib' for a library member, else the object)."""
    out = {}
    for ln in open(path, encoding='latin1'):
        if 'Static symbols' in ln:
            break
        p = ln.split()
        if len(p) >= 4 and re.fullmatch(r'[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}', p[0]) \
                and re.fullmatch(r'[0-9A-Fa-f]{8}', p[2]):
            src = p[-1]
            out[p[1]] = src.split(':', 1)[0].lower() if ':' in src else src
    return out


def bindable(name):
    return not _NOT_BINDABLE.match(name) and not name.startswith('\x7f') \
        and not name.startswith('__IMPORT_DESCRIPTOR_')


def sdk_version(sdk):
    text = open(os.path.join(sdk, 'readme.txt'), 'rb').read().decode('latin1')
    m = re.search(r'Release v([0-9][0-9.]*)', text)
    return m.group(1) if m else 'UNKNOWN'


def script_version(sdk):
    path = os.path.join(sdk, 'Scripts', 'shared', 'Source', 'Include', 'scriptversion.h')
    m = re.search(r'kCurrentScriptVersion\s*=\s*([0-9.]+)f', open(path, 'rb').read().decode('latin1'))
    return m.group(1) if m else 'UNKNOWN'


def verify_hash(path):
    """The retail hash list pins the SDK libraries; a library that differs is not the one the
    list describes."""
    rel = os.path.relpath(path, paths.REFERENCE)
    want = None
    for line in open(HASHES, encoding='utf-8'):
        h, p = line.rstrip('\n').split('  ', 1)
        if p == rel:
            want = h
    got = hashlib.sha256(open(path, 'rb').read()).hexdigest()
    if want != got:
        sys.exit(f'gen_allowlist: {os.path.basename(path)} does not match the retail hash list')
    return got


# --- the template build -------------------------------------------------------------------------

def build_template(jobs):
    """Instantiate the Single template and build it with toolchain/build_scr.sh; return the staging
    directory build_scr.py leaves (map, .scr, objects)."""
    work = os.path.join(paths.WORK, 'scr2native', 'allowlist')
    src = os.path.join(work, 'src', TEMPLATE_NAME)
    out = os.path.join(work, 'out', TEMPLATE_NAME + '.scr')
    scr = os.path.join(REPO, 'toolchain', 'scr')
    subprocess.run([sys.executable, '-I', os.path.join(scr, 'new_from_template.py'), TEMPLATE,
                    TEMPLATE_NAME, src], check=True)
    env = dict(os.environ, SFCOP_WORK=work)
    subprocess.run(['nice', '-n', '19', 'sh', os.path.join(REPO, 'toolchain', 'build_scr.sh'), src,
                    out, '--jobs', str(jobs)], check=True, env=env)
    return os.path.join(work, 'toolchain', 'scr', TEMPLATE_NAME)


def runtime_definitions():
    """Every external symbol the VC6 C and C++ runtime libraries define."""
    libdir = os.path.join(paths.TOOLS, 'vc6-lib', os.environ.get('MSVC_LIB_SET', 'initial'))
    if not os.path.isdir(libdir):
        sys.exit(f'gen_allowlist: no VC6 Lib set at {libdir} (toolchain/vc6-lib/fetch.sh)')
    by_upper = {f.upper(): f for f in os.listdir(libdir)}
    out = set()
    for name in RUNTIME_LIB_FILES:
        if name not in by_upper:
            sys.exit(f'gen_allowlist: {name} is missing from {libdir}')
        for _, body in ar_members(os.path.join(libdir, by_upper[name])):
            out |= coff_externals(body)[0]
    return out


def template_files(build_dir):
    maps = [f for f in os.listdir(build_dir) if f.lower().endswith('.map')]
    if len(maps) != 1:
        sys.exit(f'gen_allowlist: expected one link map in {build_dir}, found {len(maps)}')
    name = maps[0][:-4]
    scr = os.path.join(build_dir, name + '.scr')
    objdir = os.path.join(build_dir, 'obj')
    objs = sorted(os.path.join(objdir, f) for f in os.listdir(objdir) if f.lower().endswith('.obj'))
    return os.path.join(build_dir, maps[0]), scr, objs


# --- generation ---------------------------------------------------------------------------------

def generate(build_dir):
    """The allow-list parts, each a set of names, and the facts for the header."""
    sdk = sdk_dir()
    lib = os.path.join(sdk, 'Scripts', 'Libraries', 'ScriptInterfaceR.lib')
    lib_hash = verify_hash(lib)
    map_path, scr_path, objs = template_files(build_dir)

    library, refs = set(), set()
    for _, body in ar_members(lib):
        d, u = coff_externals(body)
        library |= d
        refs |= u
    for obj in objs:
        d, u = coff_externals(open(obj, 'rb').read())
        if os.path.basename(obj).lower() != SHIM_OBJ:
            refs |= u
    refs = {r[len('__imp_'):] if r.startswith('__imp_') else r for r in refs} - library

    publics = map_publics(map_path)
    linked, runtime, unresolved = set(), set(), set()
    for r in refs:
        src = publics.get(r)
        if src is None:
            unresolved.add(r)
        elif src in SDK_LIBS or src.lower() == SHIM_OBJ:
            linked.add(r)
        elif src in RUNTIME_LIBS:
            runtime.add(r)
    # A member the template did not link still belongs to the library; its runtime references are
    # resolved against the runtime libraries themselves.
    crt = runtime_definitions()
    for r in sorted(unresolved):
        if r in crt:
            runtime.add(r)
            unresolved.discard(r)
    imports = set(pe_imports(scr_path))
    parts = {'library': {s for s in library if bindable(s)},
             'linked': {s for s in linked if bindable(s)},
             'runtime': {s for s in runtime if bindable(s)},
             'imports': imports}
    facts = {'sdk': sdk_version(sdk), 'script': script_version(sdk), 'lib_sha256': lib_hash,
             'lib_set': os.environ.get('MSVC_LIB_SET', 'initial'), 'unresolved': len(unresolved)}
    return parts, facts


def render(parts, facts):
    symbols = sorted(set().union(*parts.values()))
    counts = ', '.join(f'{k} {len(v)}' for k, v in parts.items())
    head = [
        '# The symbols a native mission module may import (DESIGN §6.4; src/app/scripts/ModuleContract.md).',
        f'# Generated by {GENERATOR}; do not edit. Regenerate it from the install instead.',
        f'# Source: the SDK, release R{facts["sdk"]}, scripting library version {facts["script"]},',
        f'#   ScriptInterfaceR.lib sha256 {facts["lib_sha256"]};',
        f'#   the Single template built with VC6 SP4 (sfcop-msvc6), VC6 Lib set {facts["lib_set"]}.',
        f'# Parts (a symbol may be in more than one): {counts}.',
        f'# References the template link left unresolved, dropped: {facts["unresolved"]}.',
        f'# Symbols: {len(symbols)}. Sorted by byte value, one per line; DLL imports are written DLL!Name.',
    ]
    return '\n'.join(head + symbols) + '\n'


# --- the check ----------------------------------------------------------------------------------

def read_symbols(path):
    """The symbols of an allow-list or an import list: one per line, '#' lines and blanks skipped."""
    out = []
    for line in open(path, encoding='utf-8'):
        s = line.rstrip('\n')
        if s and not s.startswith('#'):
            out.append(s)
    return out


def foreign_imports(imports, allowed):
    """The imports not on the allow-list, in import order, each once."""
    allowed = set(allowed)
    seen, out = set(), []
    for s in imports:
        if s not in allowed and s not in seen:
            seen.add(s)
            out.append(s)
    return out


def refusal(module, symbol):
    return REFUSAL.format(module=module, symbol=symbol)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--build-dir', help='an existing template build (staging dir of build_scr.py)')
    ap.add_argument('--out', default=ALLOWLIST)
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--check', metavar='IMPORTS', help='check an import list against the allow-list')
    ap.add_argument('--allowlist', default=ALLOWLIST)
    ap.add_argument('--module', help='module name for the refusal message (default: the file name)')
    args = ap.parse_args(argv)

    if args.check:
        module = args.module or os.path.splitext(os.path.basename(args.check))[0]
        foreign = foreign_imports(read_symbols(args.check), read_symbols(args.allowlist))
        for s in foreign:
            print(refusal(module, s))
        if not foreign:
            print(f'ok: {module}: every import is on the allow-list')
        return 1 if foreign else 0

    build_dir = os.path.abspath(args.build_dir) if args.build_dir else build_template(args.jobs)
    parts, facts = generate(build_dir)
    text = render(parts, facts)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    n = sum(1 for line in text.splitlines() if not line.startswith('#'))
    print(f'{args.out}: {n} symbols ({", ".join(f"{k} {len(v)}" for k, v in parts.items())}); '
          f'{facts["unresolved"]} references the template link did not resolve were left out')
    return 0


if __name__ == '__main__':
    sys.exit(main())
