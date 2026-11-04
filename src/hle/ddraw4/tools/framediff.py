#!/usr/bin/env python3
"""The percent of pixels that differ between pairs of frames, for the DirectDraw and Direct3D
modules' proof run (src/hle/ddraw4/README.md).

    python3 framediff.py A B [--tolerance N]
    python3 framediff.py --self-test

A and B are two BMP files, or two folders of them (the boot host's --shots output). Folders are
paired in name order, the first frame of A with the first of B, and so on, up to the shorter
folder; the shot names carry the run's time, so the two runs' names differ. A pixel differs when
any channel differs by more than the tolerance (default 0). Frames of different sizes differ
everywhere. One line per pair: both names and the percent, then the largest percent.

Reads the uncompressed 24- and 32-bit BMPs the boot host writes; standard library only.
"""
import argparse
import os
import struct
import sys


def read_bmp(data):
    """(width, height, rows) from BMP bytes; rows top first, each a list of (r, g, b)."""
    if data[:2] != b'BM':
        raise ValueError('not a BMP')
    offset = struct.unpack_from('<I', data, 10)[0]
    width, height = struct.unpack_from('<ii', data, 18)
    bits, compression = struct.unpack_from('<HI', data, 28)
    if bits not in (24, 32) or compression != 0:
        raise ValueError('only uncompressed 24- and 32-bit BMPs')
    step = bits // 8
    stride = (width * step + 3) & ~3
    rows = []
    for y in range(abs(height)):
        start = offset + y * stride
        rows.append([(data[start + x * step + 2], data[start + x * step + 1], data[start + x * step])
                     for x in range(width)])
    if height > 0:  # stored bottom up
        rows.reverse()
    return width, abs(height), rows


def percent_different(a, b, tolerance=0):
    """The percent of pixels of frame a that differ from frame b."""
    wa, ha, rows_a = a
    wb, hb, rows_b = b
    if (wa, ha) != (wb, hb):
        return 100.0
    if wa * ha == 0:
        return 0.0
    differ = 0
    for row_a, row_b in zip(rows_a, rows_b):
        for pa, pb in zip(row_a, row_b):
            if any(abs(ca - cb) > tolerance for ca, cb in zip(pa, pb)):
                differ += 1
    return 100.0 * differ / (wa * ha)


def frames(path):
    if os.path.isdir(path):
        return [os.path.join(path, name) for name in sorted(os.listdir(path))
                if name.lower().endswith('.bmp')]
    return [path]


def load(path):
    with open(path, 'rb') as f:
        return read_bmp(f.read())


def write_bmp(width, height, pixels):
    """BMP bytes for a list of (r, g, b) rows, top first (the self-test's frames)."""
    stride = (width * 3 + 3) & ~3
    body = b''
    for row in reversed(pixels):
        line = b''.join(bytes((b, g, r)) for r, g, b in row)
        body += line + b'\0' * (stride - len(line))
    header = struct.pack('<2sIHHI', b'BM', 54 + len(body), 0, 0, 54)
    info = struct.pack('<IiiHHIIiiII', 40, width, height, 1, 24, 0, len(body), 0, 0, 0, 0)
    return header + info + body


def self_test():
    black, white, grey = (0, 0, 0), (255, 255, 255), (2, 2, 2)
    a = read_bmp(write_bmp(2, 2, [[black, white], [white, black]]))
    assert a[2] == [[black, white], [white, black]], a
    b = read_bmp(write_bmp(2, 2, [[black, white], [white, grey]]))
    assert percent_different(a, a) == 0.0
    assert percent_different(a, b) == 25.0
    assert percent_different(a, b, tolerance=2) == 0.0
    c = read_bmp(write_bmp(3, 2, [[black] * 3, [black] * 3]))
    assert percent_different(a, c) == 100.0
    print('self-test passed')
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('a', nargs='?')
    parser.add_argument('b', nargs='?')
    parser.add_argument('--tolerance', type=int, default=0)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()
    if not args.a or not args.b:
        parser.error('two files or folders')
    pairs = list(zip(frames(args.a), frames(args.b)))
    if not pairs:
        print('no frames to compare')
        return 1
    worst = 0.0
    for path_a, path_b in pairs:
        diff = percent_different(load(path_a), load(path_b), args.tolerance)
        worst = max(worst, diff)
        print('%s\t%s\t%.3f%%' % (os.path.basename(path_a), os.path.basename(path_b), diff))
    print('largest: %.3f%% over %d pairs' % (worst, len(pairs)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
