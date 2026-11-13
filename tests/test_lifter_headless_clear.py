"""The software Direct3D clear of cmake/lifter/patches/pcrecomp-headless-clear.patch: fill16, fill32
and clear_rect, taken from the patch text, compiled with the host C compiler and run on small
surfaces whose expected pixels are drawn below by hand.

Every surface starts filled with the byte 0xEE and has one pixel of padding past its width, so a
write past a row's end shows. A picture row has one character per pixel, padding included:
'.' untouched, 'C' the case's colour, '0' zero, 'H' half depth (0x7FFF), 'F' far (0xFFFF).
"""
import pathlib
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parent.parent
PATCH = ROOT / 'cmake' / 'lifter' / 'patches' / 'pcrecomp-headless-clear.patch'
CC = shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')

pytestmark = pytest.mark.skipif(CC is None, reason='no C compiler (cc, gcc or clang) on this host')


def patched_functions():
    """The block of lines the patch adds in front of clear(): fill16, fill32 and clear_rect."""
    lines = PATCH.read_text().splitlines()
    first = next(i for i, line in enumerate(lines) if line.startswith('+static void fill16('))
    while lines[first - 1].startswith('+'):  # the comment above fill16
        first -= 1
    last = first
    while lines[last + 1].startswith('+'):
        last += 1
    return '\n'.join(line[1:] for line in lines[first:last + 1])


# name: (bpp, width, height, z buffer (width, height) or None, rect (l, t, r, b), flags, colour, z)
CASES = {
    'zero_width': (16, 4, 2, None, (2, 0, 2, 2), 1, 0x1234, 0.0),
    'inverted': (32, 4, 2, None, (3, 1, 1, 0), 1, 0x11223344, 0.0),
    'one_pixel_16': (16, 4, 3, None, (1, 1, 2, 2), 1, 0x1234, 0.0),
    'one_pixel_32': (32, 4, 3, None, (3, 2, 4, 3), 1, 0x11223344, 0.0),
    'unaligned_16': (16, 7, 2, None, (1, 0, 6, 2), 1, 0x1234, 0.0),
    'unaligned_16_memset': (16, 7, 1, None, (2, 0, 5, 1), 1, 0xABAB, 0.0),
    'full_rows_32': (32, 5, 2, None, (0, 0, 5, 2), 1, 0x11223344, 0.0),
    'full_rows_32_zero': (32, 5, 2, None, (0, 0, 5, 2), 1, 0x00000000, 0.0),
    'clip_left_top': (16, 4, 3, None, (-2, -1, 2, 2), 1, 0x1234, 0.0),
    'clip_right_bottom': (32, 4, 3, None, (2, 1, 9, 7), 1, 0x11223344, 0.0),
    'other_bpp_untouched': (24, 4, 2, None, (0, 0, 4, 2), 1, 0x1234, 0.0),
    'no_target_flag': (16, 4, 2, None, (0, 0, 4, 2), 0, 0x1234, 0.0),
    'depth_half_smaller_buffer': (16, 4, 3, (3, 2), (1, 0, 4, 3), 2, 0, 0.5),
    'depth_clamped_far': (16, 3, 2, (3, 2), (0, 1, 3, 2), 2, 0, 7.0),
    'depth_clamped_near': (32, 3, 2, (3, 2), (-1, -1, 2, 1), 2, 0, -3.0),
    'colour_and_depth': (16, 3, 2, (3, 2), (1, 0, 3, 2), 3, 0x1234, 1.0),
}

# The target's rows, then, after '|', the z buffer's rows when the case has one.
EXPECTED = {
    'zero_width': ['.....', '.....'],
    'inverted': ['.....', '.....'],
    'one_pixel_16': ['.....', '.C...', '.....'],
    'one_pixel_32': ['.....', '.....', '...C.'],
    'unaligned_16': ['.CCCCC..', '.CCCCC..'],
    'unaligned_16_memset': ['..CCC...'],
    'full_rows_32': ['CCCCC.', 'CCCCC.'],
    'full_rows_32_zero': ['00000.', '00000.'],
    'clip_left_top': ['CC...', 'CC...', '.....'],
    'clip_right_bottom': ['.....', '..CC.', '..CC.'],
    'other_bpp_untouched': ['.....', '.....'],
    'no_target_flag': ['.....', '.....'],
    'depth_half_smaller_buffer': ['.....', '.....', '.....', '|', '.HH.', '.HH.'],
    'depth_clamped_far': ['....', '....', '|', '....', 'FFF.'],
    'depth_clamped_near': ['....', '....', '|', '00..', '....'],
    'colour_and_depth': ['.CC.', '.CC.', '|', '.FF.', '.FF.'],
}

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct surf { int w, h, bpp, pitch; uint8_t *px; struct surf *zbuf; } surf_t;

%(functions)s

static uint8_t target[4096], depth[4096];

/* One character per pixel of each row, padding included; depth letters only for a z buffer. */
static void show(const surf_t *s, uint32_t colour, int is_depth) {
    int size = s->bpp / 8, columns = s->pitch / size;
    uint32_t mask = size == 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1;
    for (int y = 0; y < s->h; y++) {
        for (int x = 0; x < columns; x++) {
            const uint8_t *p = s->px + (size_t)y * (size_t)s->pitch + (size_t)x * (size_t)size;
            uint32_t v = 0;
            int untouched = 1;
            for (int i = 0; i < size; i++) {
                v |= (uint32_t)p[i] << (8 * i);
                untouched &= p[i] == 0xEE;
            }
            putchar(untouched ? '.' : v == 0 ? '0' : is_depth && v == 0x7FFF ? 'H'
                  : is_depth && v == 0xFFFF ? 'F' : !is_depth && v == (colour & mask) ? 'C' : '?');
        }
        putchar('\n');
    }
}

static void run(const char *name, int bpp, int w, int h, int zw, int zh, int l, int t, int r, int b,
                uint32_t flags, uint32_t colour, float z) {
    surf_t s = {w, h, bpp, (w + 1) * (bpp / 8), target, NULL};
    surf_t zb = {zw, zh, 16, (zw + 1) * 2, depth, NULL};
    memset(target, 0xEE, sizeof target);
    memset(depth, 0xEE, sizeof depth);
    if (zw > 0) s.zbuf = &zb;
    clear_rect(&s, l, t, r, b, flags, colour, z);
    printf("case %%s\n", name);
    show(&s, colour, 0);
    if (zw > 0) {
        printf("|\n");
        show(&zb, 0, 1);
    }
}

int main(void) {
%(calls)s
    return 0;
}
'''


def harness():
    calls = []
    for name, (bpp, w, h, zbuf, (l, t, r, b), flags, colour, z) in CASES.items():
        zw, zh = zbuf or (0, 0)
        calls.append(f'    run("{name}", {bpp}, {w}, {h}, {zw}, {zh}, {l}, {t}, {r}, {b}, {flags}u, '
                     f'0x{colour:08X}u, {z!r}f);')
    return HARNESS % {'functions': patched_functions(), 'calls': '\n'.join(calls)}


@pytest.fixture(scope='module')
def results(tmp_path_factory):
    folder = tmp_path_factory.mktemp('headless_clear')
    source, program = folder / 'clear.c', folder / 'clear'
    source.write_text(harness())
    subprocess.run([CC, '-std=c99', '-O2', '-Wall', '-Werror', str(source), '-o', str(program)],
                   check=True, capture_output=True, text=True)
    out = subprocess.run([str(program)], check=True, capture_output=True, text=True).stdout
    shutil.rmtree(folder)
    found, name = {}, None
    for line in out.splitlines():
        if line.startswith('case '):
            name = line[5:]
            found[name] = []
        else:
            found[name].append(line)
    return found


@pytest.mark.parametrize('name', list(CASES))
def test_each_clear_writes_exactly_the_pixels_drawn(results, name):
    assert results[name] == EXPECTED[name]


def test_the_patch_adds_the_fill_routines_and_clear_rect_as_one_block():
    functions = patched_functions()
    for start in ('static void fill16(', 'static void fill32(', 'static void clear_rect('):
        assert functions.count(start) == 1
    assert functions.rstrip().endswith('}')
