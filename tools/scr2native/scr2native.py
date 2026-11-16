#!/usr/bin/env python3
"""Translate a mission script (.scr) into a native mission module (DESIGN §5.9 path (b)).

    scr2native.py IN.scr -o ROOT [--base ADDR] [--work DIR] [--pcrecomp DIR] [--jobs N]
                  [--check REFERENCE --trace ACTUAL]

ROOT is the modules root: the module is written to ROOT/<Name>/<Name>.dylib (.so on Linux) with
its module.json (src/app/scripts/ModuleContract.md §4). `-o` and `--out` are the same option.

The steps (tools/scr2native/README.md):

  read      IN.scr is a PE32 DLL for i386 that exports MissionSpecEntry and ScriptEntry
  check     each Windows import (DLL!Name) is on src/app/scripts/allowlist.txt; a foreign one stops
            the translation with the loader's refusal message, and nothing is written
  rebase    the DLL is moved to its guest base: slot n of SFCOP_GUEST_MODULE_FIRST for the n-th .scr
            of its folder (GuestModuleAbi.h), or --base; every base relocation is applied
  lift      pcrecomp, as the game is lifted (cmake/lifter/run_lift.py), in the lifter container,
            with cmake/lifter/patches/series applied to a copy of the clone named by its patch set
            (<work>/.pcrecomp, cmake/lifter/pcrecomp_copy.py; the clone is only read):
            every function of the DLL, the scripting library and the C runtime it links included,
            to C at the rebased addresses; seeded with the exports, RTTI and the code pointers its
            relocations name in data
  compile   the lifted C with the host compiler, with module_rt.h first (the machine is the
            client's) and module_glue.c (the binding, the version tag, the two exports), plus
            module_image.c (the image, the import slots, the guest image descriptor)
  verify    every undefined symbol of the library is an allowed sfcop_import: or on the host
            baseline list (ModuleContract.md §3), as the loader will check it
  package   ROOT/<Name>/: the library and module.json, written only when every step passed

--check holds the module to a reference: ACTUAL is the oracle trace of a run of the translated
module, REFERENCE the trace it must equal (oracle/trace/compare.py); on any difference no module
is written and the exit status is 4.

Exit status: 0 written; 1 refused (a foreign import, a symbol off the lists, an export not lifted);
2 an input or option this tool cannot honour; 3 a tool it needs is missing (the container, the
compiler, pcrecomp); 4 --check found a difference.

Nothing from the .scr is written outside --work and ROOT. Standard library only.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
# The decompilation's harness/ (decomp/docs/INTERFACE.md), as gen_allowlist.py imports it.
sys.path.insert(0, os.path.abspath(os.environ.get('SFCOP_DECOMP_DIR') or os.path.join(REPO, 'decomp')))
sys.path.insert(0, os.path.join(REPO, 'cmake', 'lifter'))
import gen_allowlist  # noqa: E402
import pcrecomp_copy  # noqa: E402
import pe32  # noqa: E402
from harness.lib import paths  # noqa: E402

TRANSLATOR_VERSION = '1.2.1'
RUNTIME = os.path.join(HERE, 'runtime')
SCRIPTS = os.path.join(REPO, 'src', 'app', 'scripts')
ABI_HEADER_DIR = os.path.join(SCRIPTS, 'guest')
LOADER_HEADER = os.path.join(SCRIPTS, 'ModuleLoader.h')
LIFTER = os.path.join(REPO, 'cmake', 'lifter')
COMPARE = os.path.join(REPO, 'oracle', 'trace', 'compare.py')

# The game's lift is compiled with these (cmake/SfcopLifter.cmake, SFCOP_RECOMP_DEFINITIONS); a
# module must see guest memory and the return marker exactly as the game does.
RECOMP_DEFINITIONS = ('RECOMP_RETADDR=0u', 'RECOMP_LOCAL_REGS', 'RECOMP_MEM_BASE=0x100000000000ull')
LIFT_FILES = 4                     # lifted .c files per module
GUEST_FIRST, GUEST_SLOT, GUEST_SLOTS = 0x60000000, 0x00100000, 256
GRANULE = 0x10000                  # the guest allocator's reservation unit
EXPORTS = ('MissionSpecEntry', 'ScriptEntry')
# The optimisation level of a module's C. A module's attach runs thousands of lifted static
# initialisers. -O2 is the level the direct runtime view (module_rt_direct.h) was measured at; on
# its own it leaves module size unchanged against -O1 and compiled in 18.1 s a module against
# 17.0 s on a 10-module sample (README).
OPTIMISATION = '-O2'

EXIT_OK, EXIT_REFUSED, EXIT_INPUT, EXIT_TOOL, EXIT_MISMATCH = 0, 1, 2, 3, 4


class Stop(Exception):
    """Ends the translation: a message and the exit status."""

    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


# --- the host ---------------------------------------------------------------------------------

def host_abi_tag():
    """kHostAbiTag of ModuleLoader.h for this machine."""
    arch = {'arm64': 'arm64', 'aarch64': 'arm64', 'x86_64': 'x86_64', 'amd64': 'x86_64'}.get(
        platform.machine().lower(), 'unknown')
    system = {'Darwin': 'macos-itanium', 'Linux': 'linux-itanium', 'Windows': 'windows-msvc'}.get(
        platform.system(), 'unknown')
    return '%s-%s' % (arch, system)


def library_suffix():
    return {'Darwin': '.dylib', 'Linux': '.so', 'Windows': '.dll'}.get(platform.system(), '.so')


def symbol_prefix():
    """What the object format puts before a C name: '_' on Mach-O."""
    return '_' if platform.system() == 'Darwin' else ''


def loader_constants():
    """The contract version and the scripting library version, read from ModuleLoader.h so the two
    cannot drift."""
    text = open(LOADER_HEADER, encoding='utf-8').read()
    contract = re.search(r'kModuleContractVersion\s*=\s*(\d+)\s*;', text)
    script = re.search(r'kScriptVersion\s*=\s*([0-9.]+)f\s*;', text)
    if not contract or not script:
        raise Stop(EXIT_TOOL, 'cannot read the contract and script versions from ModuleLoader.h')
    return int(contract.group(1)), script.group(1)


def baseline_path(tag=None):
    return os.path.join(SCRIPTS, 'baseline-%s.txt' % (tag or host_abi_tag()))


def read_list(path):
    if not os.path.isfile(path):
        return set()
    return {s for s in open(path, encoding='utf-8').read().splitlines() if s and not s.startswith('#')}


def pcrecomp_revision():
    """The revision cmake/lifter/lift.sh pins."""
    text = open(os.path.join(LIFTER, 'lift.sh'), encoding='utf-8').read()
    return re.search(r'^PCRECOMP_REV=([0-9a-f]{40})$', text, re.M).group(1)


def lifter_image():
    digest = hashlib.sha256(open(os.path.join(LIFTER, 'Dockerfile'), 'rb').read()).hexdigest()
    return 'sfcop-lifter-py:' + digest[:12]


def run(cmd, log=None, cwd=None):
    """Runs `cmd`; on failure stops with its last lines."""
    proc = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if log:
        with open(log, 'a', encoding='utf-8') as f:
            f.write('$ %s\n%s%s' % (' '.join(cmd), proc.stdout, proc.stderr))
    if proc.returncode != 0:
        tail = '\n'.join((proc.stdout + proc.stderr).strip().splitlines()[-15:])
        raise Stop(EXIT_TOOL, '%s failed (exit %d):\n%s' % (os.path.basename(cmd[0]) if cmd[0] != 'nice'
                                                             else cmd[3], proc.returncode, tail))
    return proc.stdout


# --- read, check, rebase ----------------------------------------------------------------------

def read_scr(path):
    try:
        data = open(path, 'rb').read()
    except OSError as e:
        raise Stop(EXIT_INPUT, 'cannot read %s: %s' % (path, e.strerror))
    try:
        dll = pe32.Dll(data)
        exports = dll.exports()
        imports = dll.imports()
    except pe32.PeError as e:
        raise Stop(EXIT_INPUT, '%s is not a mission script: %s' % (os.path.basename(path), e))
    for name in EXPORTS:
        if name not in exports:
            raise Stop(EXIT_INPUT, '%s does not export %s' % (os.path.basename(path), name))
    return data, dll, exports, imports


def check_imports(name, imports, allowed):
    """The loader's refusal for the first import off the allow-list, in import order."""
    for imp in imports:
        if imp.full not in allowed:
            raise Stop(EXIT_REFUSED, gen_allowlist.refusal(name, imp.full))


def guest_base(path, explicit):
    """The module's guest base: --base, or its slot by its place among its folder's .scr files."""
    if explicit is not None:
        if explicit % GRANULE or not 0x20000000 <= explicit < 0x7F000000:
            raise Stop(EXIT_INPUT, '--base 0x%08X: not a 64 KB boundary in 0x20000000-0x7F000000' % explicit)
        return explicit
    folder = os.path.dirname(os.path.abspath(path))
    names = sorted((n for n in os.listdir(folder) if n.lower().endswith('.scr')),
                   key=lambda n: n.encode('utf-8'))
    index = names.index(os.path.basename(path))
    if index >= GUEST_SLOTS:
        raise Stop(EXIT_INPUT, '%s is .scr number %d of its folder; give it a --base' % (
            os.path.basename(path), index + 1))
    return GUEST_FIRST + GUEST_SLOT * index


def image_size(dll, base, explicit):
    size = (dll.size_of_image + GRANULE - 1) // GRANULE * GRANULE
    if explicit is None and size > GUEST_SLOT:
        raise Stop(EXIT_INPUT, 'the image is 0x%X bytes, more than a slot; give it a --base' % size)
    if base + size > 0x7F000000:
        raise Stop(EXIT_INPUT, 'the image at 0x%08X runs into the host rows at 0x7F000000' % base)
    return size


# --- lift -------------------------------------------------------------------------------------

def patched_pcrecomp(args):
    """The clone (--pcrecomp) with cmake/lifter/patches/series applied, as the game's build lifts
    with: a copy in <work>/.pcrecomp named by its patch set, made once and kept, so translations
    from checkouts with different patch sets never share one. A missing clone or a patch that does
    not apply is a missing tool."""
    if getattr(args, 'patched', None) is None:
        try:
            args.patched = pcrecomp_copy.patched_copy(args.pcrecomp, os.path.join(args.work, '.pcrecomp'),
                                                      LIFTER)[0]
        except pcrecomp_copy.CopyError as e:
            raise Stop(EXIT_TOOL, str(e))
    return args.patched


def lift(work, name, rebased, exports, pcrecomp, log):
    """Lifts the rebased DLL into work/out/gen, unless an identical lift is already there."""
    game = os.path.join(work, 'game', name + '.scr')
    seeds = sorted(set(rebased.code_pointer_seeds()) |
                   {rebased.image_base + rva for rva in exports.values()} |
                   {rebased.image_base + rebased.entry_rva})
    key_parts = [pe32.sha256(rebased.data), pcrecomp_revision(), pcrecomp_copy.patch_set_key(LIFTER),
                 str(LIFT_FILES),
                 ','.join('%X' % s for s in seeds)]
    for f in ('run_lift.py', 'eh_frames.py', 'Dockerfile'):
        key_parts.append(hashlib.sha256(open(os.path.join(LIFTER, f), 'rb').read()).hexdigest())
    key = hashlib.sha256('\n'.join(key_parts).encode()).hexdigest()
    key_file = os.path.join(work, 'lift.key')
    gen = os.path.join(work, 'out', 'gen')
    if (os.path.isfile(key_file) and open(key_file).read().strip() == key
            and os.path.isfile(os.path.join(gen, 'recomp_dispatch.c'))):
        return gen
    if not os.path.isfile(os.path.join(pcrecomp, 'tools', 'lift', 'lift32.py')):
        raise Stop(EXIT_TOOL, 'no pcrecomp clone at %s (cmake/lifter/setup.sh; --pcrecomp)' % pcrecomp)
    if not shutil.which('docker'):
        raise Stop(EXIT_TOOL, 'docker is not installed; the lift runs pcrecomp in the lifter container')
    image = lifter_image()
    if subprocess.run(['docker', 'image', 'inspect', image], capture_output=True).returncode != 0:
        run(['docker', 'build', '-q', '-t', image, LIFTER], log)
    for sub in ('game', 'cat', 'out'):
        os.makedirs(os.path.join(work, sub), exist_ok=True)
    if os.path.exists(key_file):
        os.remove(key_file)
    with open(game, 'wb') as f:
        f.write(rebased.data)
    shutil.rmtree(gen, ignore_errors=True)

    def container(*cmd):
        run(['nice', '-n', '19', 'docker', 'run', '--rm', '--network', 'none', '--cpus', '1',
             '--memory', '4g', '-v', work + ':/w', '-v', pcrecomp + ':/p:ro', '-v', LIFTER + ':/s:ro',
             '-e', 'PCRECOMP_TOOLS=/p/tools', '-w', '/w', image] + list(cmd), log)

    rel = 'game/%s.scr' % name
    container('python', '-I', '/p/tools/cpp/rtti.py', rel, '-o', 'cat/rtti.json', '--seeds',
              'cat/rtti_seeds.json')
    rtti = json.load(open(os.path.join(work, 'cat', 'rtti_seeds.json')))
    merged = sorted({e['address'] for e in rtti} | set(seeds))
    json.dump([{'address': a, 'address_hex': '0x%08X' % a} for a in merged],
              open(os.path.join(work, 'cat', 'seeds.json'), 'w'), indent=0)
    container('python', '-I', '/p/tools/disasm/disasm32.py', rel, '-o', 'cat/functions.json',
              '--seed-functions', 'cat/seeds.json')
    container('python', '-I', '/s/run_lift.py', '--exe', rel, '--catalog', 'cat/functions.json',
              '--seeds', 'cat/seeds.json', '--all', '--out', 'out/gen', '--files', str(LIFT_FILES))
    with open(key_file, 'w') as f:
        f.write(key + '\n')
    return gen


def dispatch_entries(gen):
    """{address: name} of the lifted functions, from recomp_dispatch.c."""
    text = open(os.path.join(gen, 'recomp_dispatch.c'), encoding='utf-8').read()
    return {int(a, 16): n for a, n in re.findall(r'\{ 0x([0-9A-F]{8})u, (\w+) \}', text)}


# --- emit and compile -------------------------------------------------------------------------

def c_bytes(data):
    out, line = [], []
    for i, b in enumerate(data):
        line.append('%d' % b)
        if len(line) == 32:
            out.append(','.join(line))
            line = []
    if line:
        out.append(','.join(line))
    return ',\n'.join(out) if out else '0'


def import_symbol(full):
    return 'sfcop_import:' + full


def emit_image(path, rebased, base, size, imports, exports, dispatch):
    entry = rebased.image_base + rebased.entry_rva if rebased.entry_rva else 0
    weak = '__attribute__((weak_import))' if platform.system() == 'Darwin' else '__attribute__((weak))'
    lines = ['/* Generated by tools/scr2native/scr2native.py: the guest image of one translated',
             ' * mission. Not committed: it holds the original script\'s sections. */',
             '#include <stdint.h>', '#include "GuestModuleAbi.h"', '',
             'const char *sfcop_bind(sfcop_guest_resolver resolve);',
             'uint32_t sfcop_call(uint32_t address, uint32_t argc, const uint32_t *args);',
             'extern const sfcop_guest_dispatch recomp_dispatch_table[];', '']
    segs = []
    for i, (rva, seg_size, raw) in enumerate(rebased.image_segments()):
        raw = raw.rstrip(b'\0')
        lines.append('static const uint8_t seg%d[] = {\n%s};' % (i, c_bytes(raw)))
        segs.append('{0x%08Xu, 0x%08Xu, %du, seg%d}' % (rva, seg_size, len(raw), i))
    lines.append('static const sfcop_guest_segment segments[] = {%s};' % ', '.join(segs))
    # Each Windows import is also an undefined weak symbol of the library, so the loader's check
    # of the file (ModuleContract.md §3) sees every import the image will be bound to.
    for i, imp in enumerate(imports):
        lines.append('extern char imp%d __asm__("%s%s") %s;' % (i, symbol_prefix(), import_symbol(imp.full), weak))
    lines.append('__attribute__((used)) static const void *const import_refs[] = {%s};' % (
        ', '.join('&imp%d' % i for i in range(len(imports))) or '0'))
    lines.append('static const sfcop_guest_import imports[] = {%s};' % (', '.join(
        '{0x%08Xu, "%s"}' % (imp.iat_rva, imp.full) for imp in imports) or '{0, 0}'))
    lines.append('__attribute__((visibility("default"))) const sfcop_guest_image sfcop_module_guest = {')
    lines.append('    SFCOP_GUEST_ABI_VERSION, 0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu,' % (
        base, size, entry, base + exports['MissionSpecEntry'], base + exports['ScriptEntry']))
    lines.append('    segments, %du, imports, %du, recomp_dispatch_table, %du, sfcop_bind, sfcop_call};' % (
        len(segs), len(imports), len(dispatch)))
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


def compile_flags(pcrecomp, gen=None, lifted=False):
    """The host compiler's flags for a C file of a module (gen_baseline.py compiles its probes
    with the same). A lifted file (recomp_*.c) also gets module_rt_direct.h, which gives it a
    const view of the runtime pointers; the glue and the image, which define and fill them, do
    not."""
    contract, script = loader_constants()
    flags = ['-c', '-w', OPTIMISATION, '-fPIC', '-fno-strict-aliasing', '-fwrapv', '-fcommon',
             '-fno-lto', '-fvisibility=hidden', '-include', os.path.join(RUNTIME, 'module_rt.h')]
    if lifted:
        flags += ['-include', os.path.join(RUNTIME, 'module_rt_direct.h')]
    if gen:
        flags += ['-I', gen]
    flags += ['-I', os.path.join(pcrecomp, 'runtime', 'recomp32'), '-I', ABI_HEADER_DIR]
    flags += ['-D' + d for d in RECOMP_DEFINITIONS]
    flags += ['-DSFCOP_ABI_TAG="%s"' % host_abi_tag(), '-DSFCOP_SCRIPT_VERSION=%sf' % script,
              '-DSFCOP_CONTRACT_VERSION=%du' % contract]
    return flags


def compile_module(work, name, gen, imports, pcrecomp, jobs, log):
    """Compiles and links the module; returns the library's path in work/build."""
    cc = os.environ.get('CC', 'cc')
    if not shutil.which(cc):
        raise Stop(EXIT_TOOL, 'no C compiler (%s)' % cc)
    build = os.path.join(work, 'build')
    os.makedirs(build, exist_ok=True)
    lifted = sorted(os.path.join(gen, f) for f in os.listdir(gen)
                    if f.startswith('recomp_') and f.endswith('.c'))
    sources = lifted + [os.path.join(RUNTIME, 'module_glue.c'), os.path.join(work, 'module_image.c')]
    objects = []

    def one(src):
        obj = os.path.join(build, os.path.basename(src)[:-2] + '.o')
        flags = compile_flags(pcrecomp, gen, lifted=src in lifted)
        run(['nice', '-n', '19', cc] + flags + [src, '-o', obj], log)
        return obj

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        objects = list(pool.map(one, sources))
    lib = os.path.join(build, name + library_suffix())
    exported = [symbol_prefix() + s for s in EXPORTS + ('sfcop_module_abi', 'sfcop_module_guest')]
    if platform.system() == 'Darwin':
        listing = os.path.join(build, 'exports.txt')
        with open(listing, 'w') as f:
            f.write('\n'.join(exported) + '\n')
        link = [cc, '-dynamiclib', '-o', lib, '-install_name', '@rpath/%s%s' % (name, library_suffix()),
                '-Wl,-exported_symbols_list,' + listing]
        for imp in imports:
            link.append('-Wl,-U,%s%s' % (symbol_prefix(), import_symbol(imp.full)))
    else:
        script_path = os.path.join(build, 'exports.map')
        with open(script_path, 'w') as f:
            f.write('{ global: %s; local: *; };\n' % '; '.join(exported))
        link = [cc, '-shared', '-o', lib, '-Wl,--version-script=' + script_path]
    run(link + objects, log)
    return lib


def undefined_symbols(lib):
    """The library's undefined dynamic symbols, the object format's prefix removed, in file order."""
    out = run(['nm', '-u', lib])
    pre = symbol_prefix()
    names = []
    for line in out.splitlines():
        s = line.strip()
        if s and not s.endswith(':'):
            names.append(s[len(pre):] if pre and s.startswith(pre) else s)
    return names


def verify(name, lib, allowed, baseline):
    """The loader's import check (ModuleContract.md §3) on the built library."""
    prefix = 'sfcop_import:'
    for s in undefined_symbols(lib):
        if s.startswith(prefix):
            if s[len(prefix):] not in allowed:
                raise Stop(EXIT_REFUSED, gen_allowlist.refusal(name, s[len(prefix):]))
        elif s not in baseline:
            raise Stop(EXIT_REFUSED, gen_allowlist.refusal(name, s))


# --- package and check ------------------------------------------------------------------------

def package(root, name, lib, digest):
    staging = os.path.join(root, '.%s.staging' % name)
    shutil.rmtree(staging, ignore_errors=True)
    os.makedirs(staging)
    shutil.copy2(lib, os.path.join(staging, os.path.basename(lib)))
    with open(os.path.join(staging, 'module.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump({'name': name, 'source_sha256': digest, 'translator_version': TRANSLATOR_VERSION},
                  f, indent=2)
        f.write('\n')
    return staging


def compare_traces(reference, actual):
    """(exit status, report) of oracle/trace/compare.py."""
    proc = subprocess.run([sys.executable, '-I', COMPARE, reference, actual],
                          capture_output=True, text=True)
    return proc.returncode, (proc.stdout + proc.stderr).strip()


def install(root, name, staging):
    final = os.path.join(root, name)
    old = os.path.join(root, '.%s.old' % name)
    shutil.rmtree(old, ignore_errors=True)
    if os.path.exists(final):
        os.rename(final, old)
    os.rename(staging, final)
    shutil.rmtree(old, ignore_errors=True)
    return final


def clean_work(work):
    """Removes the lift and the objects once the module is installed; the log stays."""
    for sub in ('build', 'cat', 'game', 'out'):
        shutil.rmtree(os.path.join(work, sub), ignore_errors=True)
    for f in ('module_image.c', 'lift.key'):
        if os.path.exists(os.path.join(work, f)):
            os.remove(os.path.join(work, f))


def translate(args):
    started = time.monotonic()
    name = os.path.splitext(os.path.basename(args.input))[0]
    if not re.match(r'[A-Za-z0-9_.-]+\Z', name):
        raise Stop(EXIT_INPUT, 'the script name %r is not a module name' % name)
    data, dll, exports, imports = read_scr(args.input)
    allowed = read_list(gen_allowlist.ALLOWLIST)
    check_imports(name, imports, allowed)
    base = guest_base(args.input, args.base)
    size = image_size(dll, base, args.base)
    try:
        rebased = dll.rebased(base)
    except pe32.PeError as e:
        raise Stop(EXIT_INPUT, '%s: %s' % (name, e))
    work = os.path.join(args.work, name)
    os.makedirs(work, exist_ok=True)
    log = os.path.join(work, 'scr2native.log')
    open(log, 'w').close()
    pcrecomp = patched_pcrecomp(args)
    lift_started = time.monotonic()
    gen = lift(work, name, rebased, exports, pcrecomp, log)
    lift_seconds = time.monotonic() - lift_started
    dispatch = dispatch_entries(gen)
    for export, rva in sorted(exports.items()):
        if export in EXPORTS and base + rva not in dispatch:
            raise Stop(EXIT_REFUSED, '%s: the export %s at 0x%08X was not lifted' % (name, export, base + rva))
    if dll.entry_rva and base + dll.entry_rva not in dispatch:
        raise Stop(EXIT_REFUSED, '%s: the entry point at 0x%08X was not lifted' % (name, base + dll.entry_rva))
    emit_image(os.path.join(work, 'module_image.c'), rebased, base, size, imports, exports, dispatch)
    compile_started = time.monotonic()
    lib = compile_module(work, name, gen, imports, pcrecomp, args.jobs, log)
    compile_seconds = time.monotonic() - compile_started
    verify(name, lib, allowed, read_list(baseline_path()))
    os.makedirs(args.out, exist_ok=True)
    staging = package(args.out, name, lib, pe32.sha256(data))
    if args.check:
        status, report = compare_traces(args.check, args.trace)
        if status != 0:
            shutil.rmtree(staging, ignore_errors=True)
            raise Stop(EXIT_MISMATCH if status == 4 else EXIT_INPUT,
                       '%s: %s\n%s: the trace differs from the reference; no module written'
                       % (name, report, name) if status == 4 else '%s: %s' % (name, report))
    final = install(args.out, name, staging)
    if not args.keep_work:
        clean_work(work)
    lib_size = os.path.getsize(os.path.join(final, os.path.basename(lib)))
    with open(os.path.join(work, 'result.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump({'functions': len(dispatch), 'seconds': round(time.monotonic() - started, 1),
                   'lift_seconds': round(lift_seconds, 1), 'compile_seconds': round(compile_seconds, 1)}, f)
    print('scr2native: %s: 0x%08X, %d functions, %d imports, %s, %d bytes' % (
        name, base, len(dispatch), len(imports), os.path.basename(lib), lib_size))
    return EXIT_OK


# --- all the shipped scripts ------------------------------------------------------------------

def library_functions(lib):
    """The lifted functions in a built module: its local sub_<address> symbols."""
    out = run(['nm', lib])
    return len(re.findall(r' [tT] _?sub_[0-9A-F]{8}$', out, re.M))


def current_module(root, name, digest, allowed, baseline):
    """The library of an installed module that is current for this script, else None. Current means
    its module.json names this script's hash and this translator version, and the library still
    passes verify() against today's allow-list and baseline: no lift is needed to tell."""
    folder = os.path.join(root, name)
    lib = os.path.join(folder, name + library_suffix())
    try:
        with open(os.path.join(folder, 'module.json'), encoding='utf-8') as f:
            manifest = json.load(f)
    except (OSError, ValueError):
        return None
    if (manifest.get('source_sha256') != digest or manifest.get('translator_version') != TRANSLATOR_VERSION
            or not os.path.isfile(lib)):
        return None
    try:
        verify(name, lib, allowed, baseline)
    except Stop:
        return None
    return lib


def tree_bytes(path):
    return sum(os.path.getsize(os.path.join(d, f)) for d, _, files in os.walk(path) for f in files)


def results_table(rows):
    lines = ['| Script | Guest base | .scr bytes | Imports | Functions | Module bytes | Seconds | Outcome |',
             '| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |']
    for r in rows:
        lines.append('| %s | %s | %s | %s | %s | %s | %s | %s |' % (
            r['name'], r['base'], r['scr_bytes'], r['imports'], r['functions'], r['module_bytes'],
            r['seconds'], r['outcome']))
    return '\n'.join(lines)


def translate_all(args):
    """Translates every .scr of the install's Assets/Scripts, one after another, and writes the
    results table to <work>/results.md. A module that is already current is kept (current_module);
    a refused or failed script is recorded and the rest go on, except after a tool failure (exit
    3), which would repeat: the remaining scripts are then recorded as not run. Nothing is retried."""
    started = time.monotonic()
    scripts = os.path.join(args.all, 'Assets', 'Scripts')
    if not os.path.isdir(scripts):
        raise Stop(EXIT_INPUT, 'no Assets/Scripts folder in the install given to --all')
    names = sorted((n for n in os.listdir(scripts) if n.lower().endswith('.scr')), key=lambda n: n.encode())
    if not names:
        raise Stop(EXIT_INPUT, 'no .scr files in the install given to --all')
    allowed = read_list(gen_allowlist.ALLOWLIST)
    baseline = read_list(baseline_path())
    rows, status, stopped = [], EXIT_OK, False
    for index, filename in enumerate(names):
        name = os.path.splitext(filename)[0]
        path = os.path.join(scripts, filename)
        base = '0x%08X' % (GUEST_FIRST + GUEST_SLOT * index)
        row = {'name': name, 'base': base, 'scr_bytes': os.path.getsize(path), 'imports': '-',
               'functions': '-', 'module_bytes': '-', 'seconds': '-', 'outcome': 'not run'}
        rows.append(row)
        if stopped:
            continue
        try:
            data, dll, _, imports = read_scr(path)
            row['imports'] = len(imports)
            digest = pe32.sha256(data)
            lib = current_module(args.out, name, digest, allowed, baseline)
            result_file = os.path.join(args.work, name, 'result.json')
            if lib is not None:
                recorded = {}
                if os.path.isfile(result_file):
                    recorded = json.load(open(result_file, encoding='utf-8'))
                row['functions'] = recorded.get('functions') or library_functions(lib)
                row['seconds'] = recorded.get('seconds', '-')
                row['outcome'] = 'kept: module.json names this script and passes the checks'
            else:
                one = argparse.Namespace(**vars(args))
                one.input, one.base, one.check, one.trace = path, None, None, None
                translate(one)
                lib = os.path.join(args.out, name, name + library_suffix())
                recorded = json.load(open(result_file, encoding='utf-8'))
                row['functions'] = recorded['functions']
                row['seconds'] = recorded['seconds']
                row['outcome'] = 'translated'
            row['module_bytes'] = os.path.getsize(lib)
        except Stop as e:
            row['outcome'] = ('refused' if e.status == EXIT_REFUSED else 'failed (exit %d)' % e.status) + \
                ': ' + str(e).splitlines()[0][:160].replace('|', '/')
            status = status or e.status
            stopped = e.status == EXIT_TOOL
    done = [r for r in rows if r['outcome'].startswith(('kept', 'translated'))]
    modules = sum(r['module_bytes'] for r in done)
    work = sum(tree_bytes(os.path.join(args.work, r['name'])) for r in rows
               if os.path.isdir(os.path.join(args.work, r['name'])))
    elapsed = time.monotonic() - started
    totals = ('%d scripts, %d modules (%d kept, %d translated), %d not translated; modules %.1f MB, '
              'work files %.1f MB, this run %.0f s' % (
                  len(rows), len(done), sum(r['outcome'].startswith('kept') for r in done),
                  sum(r['outcome'] == 'translated' for r in done), len(rows) - len(done),
                  modules / 1e6, work / 1e6, elapsed))
    os.makedirs(args.work, exist_ok=True)
    with open(os.path.join(args.work, 'results.md'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(results_table(rows) + '\n\n' + totals + '.\n')
    print('scr2native --all: ' + totals)
    return status


def parse_args(argv):
    ap = argparse.ArgumentParser(prog='scr2native', description=__doc__.split('\n\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, allow_abbrev=False)
    ap.add_argument('input', metavar='IN.scr', nargs='?')
    ap.add_argument('--all', metavar='INSTALL_DIR',
                    help='translate every script of INSTALL_DIR/Assets/Scripts and write <work>/results.md')
    ap.add_argument('-o', '--out', required=True, metavar='ROOT', help='the modules root')
    ap.add_argument('--base', type=lambda s: int(s, 0), help='the guest base (default: its slot)')
    ap.add_argument('--work', default=os.path.join(paths.WORK, 'scr2native', 'modules'),
                    help='where the rebased DLL, the lift and the objects go')
    ap.add_argument('--pcrecomp', default=os.environ.get('SFCOP_PCRECOMP_DIR',
                                                          os.path.join(paths.WORK, 'lifter-hle', 'pcrecomp')))
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--keep-work', action='store_true',
                    help='keep the lift and the objects after the module is installed')
    ap.add_argument('--check', metavar='REFERENCE', help='the reference oracle trace')
    ap.add_argument('--trace', metavar='ACTUAL', help="the translated module's oracle trace")
    args = ap.parse_args(argv)
    if bool(args.input) == bool(args.all):
        ap.error('give one IN.scr, or --all INSTALL_DIR')
    if args.all and (args.check or args.base is not None):
        ap.error('--all does not take --check or --base')
    if bool(args.check) != bool(args.trace):
        ap.error('--check and --trace go together')
    args.out = os.path.abspath(args.out)
    if args.all:
        args.all = os.path.abspath(args.all)
    args.work = os.path.abspath(args.work)
    args.pcrecomp = os.path.abspath(args.pcrecomp)
    return args


def main(argv=None):
    args = parse_args(argv)
    try:
        return translate_all(args) if args.all else translate(args)
    except Stop as e:
        print('scr2native: %s' % e, file=sys.stderr)
        return e.status


if __name__ == '__main__':
    sys.exit(main())
