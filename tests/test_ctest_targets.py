"""scripts/ctest_targets.py: the build targets a CTest run needs, from the folder's generated
CTestTestfile.cmake files and Ninja's target list. The samples below are written by hand in the
shape CMake and Ninja give them for a lift-on folder; no CMake build is needed."""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))

import ctest_targets  # noqa: E402

B = '/work/build'

ROOT_TESTFILE = '''# CMake generated Testfile for
# Source directory: /src
# Build directory: /work/build
#
# This file includes the relevant testing commands required for
# testing this directory and lists subdirectories to be tested as well.
add_test([=[sfcop_tests]=] "/work/build/sfcop_tests" "--no-version")
set_tests_properties([=[sfcop_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/src/cmake/SfcopSources.cmake;74;add_test;/src/CMakeLists.txt;58;include;/src/CMakeLists.txt;0;")
add_test("sfcop_app_selftest" "/usr/bin/python3" "/src/packaging/macos/check_bundle.py" "--self-test" "/work/build/check-bundle-selftest")
add_test("sfcop_app_contents" "sh" "-c" "[ -d \\"\\$1\\" ] || { echo \\"skip: \\$1 is not built\\"; exit 77; }" "/usr/bin/python3" "/work/build/dist/SFCOP.app")
add_test("sfcop_guest_tests" "sh" "/src/src/app/guest/tests/run_ehtest.sh" "/work/build/sfcop_guest_tests" "/work/build/ehtest/root" "/work/build/ehtest-run")
add_test("sfcop_machine_quantum_tests" "/work/build/sfcop_machine_quantum_tests" "--no-version")
subdirs("third_party")
subdirs("server")
'''

TESTFILES = {
    B: ROOT_TESTFILE,
    B + '/third_party': 'subdirs("../_deps/sdl3-build")\n',
    B + '/_deps/sdl3-build': '# no tests\n',
    B + '/server': 'subdirs("auth")\n',
    B + '/server/auth': 'add_test("sfcop_auth_tests" "/work/build/server/auth/sfcop_auth_tests" "--no-version")\n',
}

NINJA = '\n'.join([
    'build.ninja: RERUN_CMAKE',
    'sfcop_tests: CXX_EXECUTABLE_LINKER__sfcop_tests_RelWithDebInfo',
    'sfcop_guest_tests: CXX_EXECUTABLE_LINKER__sfcop_guest_tests_RelWithDebInfo',
    'sfcop_machine_quantum_tests: CXX_EXECUTABLE_LINKER__sfcop_machine_quantum_tests_RelWithDebInfo',
    'server/auth/sfcop_auth_tests: CXX_EXECUTABLE_LINKER__sfcop_auth_tests_RelWithDebInfo',
    'sfcop_auth_tests: phony',
    'sfcop: CXX_EXECUTABLE_LINKER__sfcop_RelWithDebInfo',
    'sfcop_app: phony',
    'all: phony',
    '',
])


def read(path):
    return TESTFILES.get(os.path.dirname(path))


def sample_tests():
    return ctest_targets.ctest_tests(B, read=read)


def known():
    return ctest_targets.parse_ninja_targets(NINJA)


def test_parse_calls_unescapes_quoted_and_keeps_bracket_arguments():
    calls = ctest_targets.parse_calls(
        '# a comment with add_test("no")\n'
        'add_test([=[a b]=] "/x/y" "say \\"hi\\"" "\\$1" "p\\;q")\n'
        'subdirs("sub")\n')
    assert calls == [('add_test', ['a b', '/x/y', 'say "hi"', '$1', 'p;q']),
                     ('subdirs', ['sub'])]


def test_ctest_tests_follows_subdirs_in_order():
    assert sample_tests() == [
        ('sfcop_tests', ['/work/build/sfcop_tests', '--no-version']),
        ('sfcop_app_selftest', ['/usr/bin/python3', '/src/packaging/macos/check_bundle.py',
                                '--self-test', '/work/build/check-bundle-selftest']),
        ('sfcop_app_contents', ['sh', '-c',
                                '[ -d "$1" ] || { echo "skip: $1 is not built"; exit 77; }',
                                '/usr/bin/python3', '/work/build/dist/SFCOP.app']),
        ('sfcop_guest_tests', ['sh', '/src/src/app/guest/tests/run_ehtest.sh',
                               '/work/build/sfcop_guest_tests', '/work/build/ehtest/root',
                               '/work/build/ehtest-run']),
        ('sfcop_machine_quantum_tests', ['/work/build/sfcop_machine_quantum_tests', '--no-version']),
        ('sfcop_auth_tests', ['/work/build/server/auth/sfcop_auth_tests', '--no-version']),
    ]


def test_parse_ninja_targets():
    outputs, phonies = known()
    assert 'server/auth/sfcop_auth_tests' in outputs
    assert 'sfcop_tests' in outputs
    assert phonies == {'sfcop_auth_tests', 'sfcop_app', 'all'}


def test_plan_names_every_test_executable_and_nothing_else():
    outputs, phonies = known()
    assert ctest_targets.plan(sample_tests(), B, outputs, phonies) == [
        'sfcop_auth_tests', 'sfcop_guest_tests', 'sfcop_machine_quantum_tests', 'sfcop_tests']


def test_plan_of_a_lift_off_folder():
    files = {B: 'add_test("sfcop_tests" "/work/build/sfcop_tests" "--no-version")\n'}
    lift_off = ctest_targets.ctest_tests(B, read=lambda p: files.get(os.path.dirname(p)))
    outputs, phonies = ctest_targets.parse_ninja_targets(
        'sfcop_tests: CXX_EXECUTABLE_LINKER__sfcop_tests_RelWithDebInfo\nall: phony\n')
    assert ctest_targets.plan(lift_off, B, outputs, phonies) == ['sfcop_tests']


def test_an_output_without_a_target_of_its_name_is_named_by_path():
    outputs, phonies = ctest_targets.parse_ninja_targets('tools/runner: C_EXECUTABLE_LINKER__x\n')
    one = [('t', [B + '/tools/runner'])]
    assert ctest_targets.plan(one, B, outputs, phonies) == ['tools/runner']


def test_paths_outside_the_folder_are_ignored():
    outputs, phonies = known()
    one = [('t', ['/elsewhere/sfcop_tests', '/work/build2/sfcop', 'sfcop_tests', B])]
    assert ctest_targets.plan(one, B, outputs, phonies) == []


def test_missing_is_empty_when_everything_is_built():
    outputs, phonies = known()
    assert ctest_targets.missing(sample_tests(), B, outputs, phonies, exists=lambda p: True) == []


def test_missing_names_the_target_and_the_command():
    outputs, phonies = known()
    absent = {B + '/sfcop_machine_quantum_tests', B + '/server/auth/sfcop_auth_tests',
              B + '/ehtest-run', B + '/dist/SFCOP.app', B + '/check-bundle-selftest'}
    lines = ctest_targets.missing(sample_tests(), B, outputs, phonies, exists=lambda p: p not in absent,
                                  shown='build/macos-arm64')
    assert lines == [
        'test sfcop_machine_quantum_tests: sfcop_machine_quantum_tests is not built '
        '(target sfcop_machine_quantum_tests); build it with: '
        'cmake --build build/macos-arm64 --target sfcop_machine_quantum_tests',
        'test sfcop_auth_tests: server/auth/sfcop_auth_tests is not built (target sfcop_auth_tests); '
        'build it with: cmake --build build/macos-arm64 --target sfcop_auth_tests',
    ]


def test_missing_reports_an_executable_no_rule_makes_and_an_unavailable_test():
    outputs, phonies = known()
    two = [('stale', [B + '/old_tests', '--no-version']), ('gone', ['NOT_AVAILABLE'])]
    lines = ctest_targets.missing(two, B, outputs, phonies, exists=lambda p: False)
    assert lines == [
        'test stale: old_tests does not exist and no build rule in /work/build makes it; '
        'configure the folder again',
        'test gone: CMake marked it NOT_AVAILABLE (its target is not defined in this '
        'configuration); configure /work/build again',
    ]


def script(*args):
    return subprocess.run([sys.executable, os.path.join(ROOT, 'scripts', 'ctest_targets.py'), *args],
                          capture_output=True, text=True)


def test_a_folder_that_is_not_ninja_exits_2(tmp_path):
    (tmp_path / 'CMakeCache.txt').write_text('CMAKE_GENERATOR:INTERNAL=Unix Makefiles\n')
    r = script('plan', str(tmp_path))
    assert r.returncode == 2
    assert 'is not a Ninja build folder' in r.stdout


def test_an_unconfigured_folder_exits_2(tmp_path):
    r = script('missing', str(tmp_path))
    assert r.returncode == 2
    assert 'is not a configured build folder' in r.stdout
