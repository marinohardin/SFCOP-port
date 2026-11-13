#!/usr/bin/env python3
"""The build targets a CTest run needs, read from the build folder itself (scripts/check.sh).

    python3 scripts/ctest_targets.py plan <build folder>
    python3 scripts/ctest_targets.py missing <build folder>

The tests are those CTest runs: the add_test lines of <build folder>/CTestTestfile.cmake and of
every folder its subdirs() lines reach, the files CMake generates from each add_test in the
sources. (`ctest --show-only=json-v1` is no help here: it leaves out the command of a test whose
executable is not built yet, which is the case this is for.) A test needs every file its command
names that the folder's Ninja build produces: the executable it runs, or one it hands to a
wrapper script (`sh run_ehtest.sh $<TARGET_FILE:...>`). Which files the build produces is asked
of Ninja (`ninja -t targets all`). So a test registered anywhere is covered without a list to
keep, and nothing a test does not name is built: no lift where none is configured.

`plan` prints one Ninja target per line, sorted: the CMake target's name when the file is the
output of a target of that name, else the file's path in the folder (both build the same file).
`missing` prints one line per test that cannot run, and exits 1: a file it needs that is still
absent, with the target and the command that builds it; an executable in the folder that no
build rule makes; a test CMake marked NOT_AVAILABLE. It exits 0 and prints nothing when every
test can run. Both exit 2 when the folder is not a configured Ninja build.
"""
import os
import subprocess
import sys


def read_cache_value(build_dir, key):
    """The value of `key` in <build_dir>/CMakeCache.txt, or None."""
    prefix = key + ':'
    with open(os.path.join(build_dir, 'CMakeCache.txt'), encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith(prefix):
                return line.rstrip('\n').split('=', 1)[1]
    return None


def parse_ninja_targets(text):
    """(outputs, phonies) from `ninja -t targets all`: every output path relative to the build
    folder, and the subset whose rule is `phony` (CMake's target names among them)."""
    outputs, phonies = set(), set()
    for line in text.splitlines():
        if ': ' not in line:
            continue
        path, rule = line.rsplit(': ', 1)
        outputs.add(path)
        if rule.strip() == 'phony':
            phonies.add(path)
    return outputs, phonies


def ninja_targets(build_dir):
    """parse_ninja_targets for the folder, or None when it is not a Ninja build."""
    if read_cache_value(build_dir, 'CMAKE_GENERATOR') != 'Ninja':
        return None
    ninja = read_cache_value(build_dir, 'CMAKE_MAKE_PROGRAM') or 'ninja'
    out = subprocess.run([ninja, '-C', build_dir, '-t', 'targets', 'all'], check=True,
                         capture_output=True, text=True).stdout
    return parse_ninja_targets(out)


_ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', '0': '\0', ';': ';'}


def parse_calls(text):
    """[(command, [arguments])] for each call in a CMake script, in order: quoted arguments
    unescaped, bracket arguments taken as written, comments skipped. Enough for the files CMake
    generates (CTestTestfile.cmake); not a general CMake parser."""
    calls, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
        elif c == '#':
            i = text.find('\n', i)
            i = n if i < 0 else i
        elif c.isalpha() or c == '_':
            j = i
            while j < n and (text[j].isalnum() or text[j] == '_'):
                j += 1
            name = text[i:j]
            while j < n and text[j] in ' \t':
                j += 1
            if j >= n or text[j] != '(':
                i = j
                continue
            args, i = _parse_args(text, j + 1)
            calls.append((name.lower(), args))
        else:
            i += 1
    return calls


def _parse_args(text, i):
    """The arguments from just after an opening parenthesis to its closing one, and the index
    after it."""
    args, n, depth = [], len(text), 0
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
        elif c == ')':
            if depth == 0:
                return args, i + 1
            depth -= 1
            i += 1
        elif c == '(':
            depth += 1
            i += 1
        elif c == '"':
            i += 1
            out = []
            while i < n and text[i] != '"':
                if text[i] == '\\' and i + 1 < n:
                    out.append(_ESCAPES.get(text[i + 1], text[i + 1]))
                    i += 2
                else:
                    out.append(text[i])
                    i += 1
            args.append(''.join(out))
            i += 1
        elif c == '[' and _bracket_open(text, i):
            eq = _bracket_open(text, i)
            close = ']' + '=' * (eq - 2) + ']'
            end = text.find(close, i + eq)
            end = n if end < 0 else end
            args.append(text[i + eq:end])
            i = end + len(close)
        else:
            j = i
            while j < n and not text[j].isspace() and text[j] not in '()"':
                j += 1
            args.append(text[i:j])
            i = j
    return args, n


def _bracket_open(text, i):
    """The length of the bracket opening `[=*[` at i, or 0."""
    j = i + 1
    while j < len(text) and text[j] == '=':
        j += 1
    return j - i + 1 if j < len(text) and text[j] == '[' else 0


def ctest_tests(build_dir, read=None):
    """[(test name, command)] for every test CTest runs in build_dir, in CTest's order: each
    CTestTestfile.cmake's own tests, then its subdirs(). A test CMake could not resolve has the
    command ['NOT_AVAILABLE']. `read(path)` returns a file's text, or None when it is absent."""
    if read is None:
        def read(path):
            try:
                with open(path, encoding='utf-8', errors='replace') as f:
                    return f.read()
            except FileNotFoundError:
                return None
    tests, seen = [], set()

    def visit(folder):
        folder = os.path.normpath(folder)
        if folder in seen:
            return
        seen.add(folder)
        text = read(os.path.join(folder, 'CTestTestfile.cmake'))
        if text is None:
            return
        subdirs = []
        for name, args in parse_calls(text):
            if name == 'add_test' and args:
                tests.append((args[0], args[1:]))
            elif name == 'subdirs':
                subdirs.extend(args)
        for sub in subdirs:
            visit(sub if os.path.isabs(sub) else os.path.join(folder, sub))

    visit(build_dir)
    return tests


def relative_in(path, build_dir):
    """path relative to build_dir when it is an absolute path inside it, else None."""
    if not os.path.isabs(path):
        return None
    root = os.path.realpath(build_dir)
    full = os.path.realpath(path)
    if full == root or not full.startswith(root + os.sep):
        return None
    return os.path.relpath(full, root)


def needed_files(tests, build_dir, outputs):
    """[(test name, path relative to build_dir)]: each file a test's command names that the
    build produces, in test and argument order, without repeats within a test."""
    needed = []
    for name, command in tests:
        seen = set()
        for arg in command:
            rel = relative_in(arg, build_dir)
            if rel is not None and rel in outputs and rel not in seen:
                seen.add(rel)
                needed.append((name, rel))
    return needed


def target_for(rel, phonies):
    """The Ninja target to name for an output: the CMake target of the file's name when there is
    one (an executable's default name, `.exe` dropped), else the path itself."""
    base = os.path.basename(rel)
    if base.endswith('.exe'):
        base = base[:-4]
    return base if base in phonies or base == rel else rel


def plan(tests, build_dir, outputs, phonies):
    """The sorted Ninja targets that build every file a test needs."""
    return sorted({target_for(rel, phonies) for _, rel in needed_files(tests, build_dir, outputs)})


def missing(tests, build_dir, outputs, phonies, exists=os.path.exists, shown=None):
    """One message per test that cannot run: a file it needs that is still absent (with the
    target and the command that builds it), an executable in the folder that no build rule
    makes, or a test CMake marked NOT_AVAILABLE. The messages name the folder as `shown`
    (default build_dir)."""
    shown = build_dir if shown is None else shown
    lines = []
    for name, command in tests:
        if command == ['NOT_AVAILABLE'] or not command:
            lines.append('test %s: CMake marked it NOT_AVAILABLE (its target is not defined in '
                         'this configuration); configure %s again' % (name, shown))
            continue
        seen = set()
        for arg in command:
            rel = relative_in(arg, build_dir)
            if rel is None or rel in seen or exists(os.path.join(build_dir, rel)):
                continue
            seen.add(rel)
            if rel in outputs:
                target = target_for(rel, phonies)
                lines.append('test %s: %s is not built (target %s); build it with: '
                             'cmake --build %s --target %s' % (name, rel, target, shown, target))
            elif arg == command[0]:
                lines.append('test %s: %s does not exist and no build rule in %s makes it; '
                             'configure the folder again' % (name, rel, shown))
    return lines


def main(argv):
    if len(argv) != 3 or argv[1] not in ('plan', 'missing'):
        sys.stderr.write(__doc__)
        return 2
    mode, build_dir = argv[1:]
    if not os.path.isfile(os.path.join(build_dir, 'CMakeCache.txt')):
        print('ctest_targets: %s is not a configured build folder' % build_dir)
        return 2
    known = ninja_targets(build_dir)
    if known is None:
        print('ctest_targets: %s is not a Ninja build folder (configure it with a preset)' % build_dir)
        return 2
    outputs, phonies = known
    folder = os.path.abspath(build_dir)
    tests = ctest_tests(folder)
    if mode == 'plan':
        for target in plan(tests, folder, outputs, phonies):
            print(target)
        return 0
    lines = missing(tests, folder, outputs, phonies, shown=build_dir)
    for line in lines:
        print(line)
    return 1 if lines else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
