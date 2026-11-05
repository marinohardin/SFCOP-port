#!/usr/bin/env python3
"""Generate the host baseline list for native mission modules (ModuleContract.md §3 rule 2).

    gen_baseline.py [--pcrecomp DIR] [--out FILE] [--check]

A translated module may import, besides the allowed `sfcop_import:` names, only what the host's
own compiler and C library give the code scr2native emits. This script derives that set from the
host toolchain and writes it to src/app/scripts/baseline-<host ABI tag>.txt, one symbol per line,
sorted by byte value, the object format's prefix removed: the list the loader reads for this host.

The set is the undefined symbols of objects compiled exactly as scr2native compiles a module
(same compiler, flags and forced header):

  runtime   pcrecomp's recomp_types.h with every inline helper emitted, and the module glue
  library   one call to each C library function that pcrecomp's lifter or runtime header emits
            (found by scanning tools/lift/lift32.py and runtime/recomp32/recomp_types.h for the
            C standard's math, string, setjmp and stdio names)
  support   what the compiler itself calls for: a large stack frame, a zeroing memset of unknown
            length, sin and cos of one value, a stack protector

The runtime the module binds by name (module_rt.h) and what each module defines for itself (the
lift's dispatch and EH tables) are left out.

--check writes nothing and exits 1 when the committed list differs from what this run would write.
Standard library only.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
import scr2native  # noqa: E402

GENERATOR = 'tools/scr2native/gen_baseline.py'

# The C standard library functions the lifter or the runtime header may emit a call to, when they
# do. The scan keeps those that pcrecomp actually names.
CANDIDATES = (
    'acos asin atan atan2 ceil cos cosh exp exp2 expm1 fabs floor fmod frexp hypot ldexp log log10 '
    'log1p log2 lrint llrint modf nearbyint pow remainder rint round sin sinh sqrt tan tanh trunc '
    'memchr memcmp memcpy memmove memset strlen strcmp setjmp longjmp fprintf printf abort'
).split()
NEEDS_STDERR = {'fprintf'}

SUPPORT = r'''
#include <math.h>
#include <string.h>
void sfcop_probe_frame(volatile char *out) { volatile char big[300000]; big[0] = out[0]; out[1] = big[299999]; }
void sfcop_probe_zero(char *p, unsigned long n) { memset(p, 0, n); }
/* A loop storing a repeated word: macOS's compiler makes it a memset_pattern16 call. */
void sfcop_probe_fill(unsigned *p, unsigned long n) { for (unsigned long i = 0; i < n; i++) p[i] = 0x01020304u; }
double sfcop_probe_sincos(double x) { return sin(x) + cos(x); }
void sfcop_probe_protect(char *s, int i) { char b[64]; b[i & 127] = s[0]; s[1] = b[(i + 1) & 63]; }
'''

RUNTIME = r'''
#define RECOMP_GENERATED_CODE
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <setjmp.h>
#define inline inline __attribute__((used))
#include "recomp_types.h"
#undef inline
void sfcop_probe_macros(uint32_t va) {
    int _fpu_cmp = 0; uint32_t _cf = 0; int _df = 1; uint32_t _flag_k = FK_NONE;
    uint32_t _flag_a = 0, _flag_b = 0; uint32_t _itail_tgt = 0;
    RECOMP_REGS_LOCALS
    RECOMP_ENTER(va);
    RECOMP_ICALL(va);
    RECOMP_ITAIL(va + 4);
    (void)_fpu_cmp; (void)_df; (void)_itail_tgt;
    RECOMP_REGS_OUT();
}
'''


def library_probe(pcrecomp):
    texts = ''
    for rel in (('tools', 'lift', 'lift32.py'), ('runtime', 'recomp32', 'recomp_types.h')):
        texts += open(os.path.join(pcrecomp, *rel), encoding='utf-8', errors='replace').read()
    used = [n for n in CANDIDATES if re.search(r'\b%s\(' % n, texts)]
    lines = ['#include <math.h>', '#include <string.h>', '#include <setjmp.h>', '#include <stdio.h>',
             '#include <stdlib.h>']
    for n in used:
        lines.append('void *sfcop_probe_%s(void) { return (void *)&%s; }' % (n, n))
        if n in NEEDS_STDERR:
            lines.append('void *sfcop_probe_stderr(void) { return (void *)&stderr; }')
    return '\n'.join(lines) + '\n', used


def compile_probe(source, name, flags, tmp, cc):
    src = os.path.join(tmp, name + '.c')
    obj = os.path.join(tmp, name + '.o')
    with open(src, 'w') as f:
        f.write(source)
    proc = subprocess.run([cc] + flags + [src, '-o', obj], capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit('gen_baseline: the %s probe does not compile:\n%s' % (name, proc.stderr[-3000:]))
    return obj


def undefined(obj):
    pre = scr2native.symbol_prefix()
    out = subprocess.run(['nm', '-u', obj], check=True, capture_output=True, text=True).stdout
    names = set()
    for line in out.splitlines():
        s = line.strip()
        if s and not s.endswith(':'):
            names.add(s[len(pre):] if pre and s.startswith(pre) else s)
    return names


def generate(pcrecomp):
    cc = os.environ.get('CC', 'cc')
    contract, script = scr2native.loader_constants()
    flags = ['-c', '-w', '-O1', '-fPIC', '-fno-strict-aliasing', '-fwrapv', '-fcommon',
             '-fvisibility=hidden', '-include', os.path.join(scr2native.RUNTIME, 'module_rt.h'),
             '-I', os.path.join(pcrecomp, 'runtime', 'recomp32'), '-I', scr2native.ABI_HEADER_DIR]
    flags += ['-D' + d for d in scr2native.RECOMP_DEFINITIONS]
    flags += ['-DSFCOP_ABI_TAG="%s"' % scr2native.host_abi_tag(), '-DSFCOP_SCRIPT_VERSION=%sf' % script,
              '-DSFCOP_CONTRACT_VERSION=%du' % contract]
    plain = [f for f in flags if f not in ('-include', os.path.join(scr2native.RUNTIME, 'module_rt.h'))]
    library, used = library_probe(pcrecomp)
    names = set()
    with tempfile.TemporaryDirectory() as tmp:
        names |= undefined(compile_probe(RUNTIME, 'runtime', flags, tmp, cc))
        glue = os.path.join(tmp, 'glue.o')
        subprocess.run([cc] + flags + [os.path.join(scr2native.RUNTIME, 'module_glue.c'), '-o', glue],
                       check=True, capture_output=True, text=True)
        names |= undefined(glue)
        names |= undefined(compile_probe(library, 'library', plain, tmp, cc))
        names |= undefined(compile_probe(SUPPORT, 'support', plain + ['-fstack-protector-all'], tmp, cc))
    # The runtime the module binds by name is not imported: module_rt.h makes each a pointer.
    rt = set(re.findall(r'X\((\w+)\)', open(os.path.join(scr2native.RUNTIME, 'module_rt.h')).read()))
    # Nor what every module defines for itself: the lift's dispatch table and its EH table.
    own = {'recomp_dispatch_table', 'recomp_dispatch_count', 'sfcop_eh_cont_table', 'sfcop_eh_cont_count'}
    names = {n for n in names if n not in rt and n not in own and not n.startswith('sfcop_')}
    version = subprocess.run([cc, '--version'], capture_output=True, text=True).stdout.splitlines()[0]
    return sorted(names, key=lambda s: s.encode()), used, version


def render(names, used, version):
    head = [
        '# The host runtime a native mission module may import on %s (ModuleContract.md §3 rule 2).'
        % scr2native.host_abi_tag(),
        '# Generated by %s; do not edit. Regenerate it with the host toolchain instead.' % GENERATOR,
        '# Compiler: %s' % version,
        '# pcrecomp %s; C library functions it emits: %s.' % (scr2native.pcrecomp_revision()[:12],
                                                             ' '.join(used)),
        '# Symbols: %d. Sorted by byte value, one per line, the object format\'s prefix removed.' % len(names),
    ]
    return '\n'.join(head + names) + '\n'


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--pcrecomp', default=scr2native.parse_args(['x', '-o', '.']).pcrecomp)
    ap.add_argument('--out', default=scr2native.baseline_path())
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args(argv)
    text = render(*generate(args.pcrecomp))
    if args.check:
        current = open(args.out, encoding='utf-8').read() if os.path.isfile(args.out) else ''
        if current != text:
            print('gen_baseline: %s is not what the host toolchain gives' % args.out, file=sys.stderr)
            return 1
        return 0
    with open(args.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print('gen_baseline: %s: %d symbols' % (os.path.relpath(args.out), text.count('\n') - 5))
    return 0


if __name__ == '__main__':
    sys.exit(main())
