#!/usr/bin/env python3
"""Gives a verdict to every value that looks like an image address but has no relocation.

Two lists come in:
  * the validator's stale_candidates.tsv: aligned data dwords outside every relocation whose
    value falls in the image;
  * the exporter's <object>.missing.tsv: 4-byte immediates inside instructions with such a
    value and no relocation.
Each entry gets the first rule that explains it as not an address, or "UNDECIDED". The
rules read only the original exe (the value and its two neighbouring dwords):

  text         the three low bytes are printable ASCII ("SHT\\0")
  utf16        each 16-bit half is 0 or printable ASCII, and so is a neighbour (L"Starfleet")
  small16      each 16-bit half is a small signed 16-bit value, -256..255 (flag and size pairs)
  constants    neither neighbour is 0 or a relocated address (it is outside the image, or a
               candidate itself): inside a run of non-address values
  round        a multiple of 0x10000 or of 100,000 (masks, 10,000,000)
  reviewed     no rule applies; candidate_notes.tsv (address, note) records the verdict of a
               review by hand, and the note is written out after "reviewed: "

Usage: candidates.py <original exe> <stale_candidates.tsv> <missing.tsv> <candidate_notes.tsv>
                     <out.tsv>
Writes where (data or code), address and verdict per entry; prints the count per verdict.
Exit status 1 if any entry is UNDECIDED.
"""
import collections
import struct
import sys

import pecoff


def printable(c):
    return 0x20 <= c < 0x7f


def is_text(v):
    return all(printable((v >> (8 * k)) & 0xff) for k in range(3))


def utf16ish(v):
    return all(h == 0 or (h < 0x100 and printable(h)) for h in (v & 0xffff, v >> 16))


def small16(v):
    return all(h < 0x100 or h >= 0xff00 for h in (v & 0xffff, v >> 16))


def verdict(v, neighbours, unrelocated, in_code):
    if is_text(v):
        return 'text'
    if not in_code and utf16ish(v) and any(utf16ish(n) for _, n in neighbours):
        return 'utf16'
    if not in_code and small16(v):
        return 'small16'
    if not in_code and all(n != 0 and unrelocated(a, n) for a, n in neighbours):
        return 'constants'
    if v % 0x10000 == 0 or v % 100000 == 0:
        return 'round'
    return 'UNDECIDED'


def read_tsv(path):
    return [line.rstrip('\n').split('\t') for line in open(path)
            if line.strip() and not line.startswith(('address', '#'))]


def main(exe_path, stale_path, missing_path, notes_path, out_path):
    pe = pecoff.PE.load(exe_path)
    lo, hi = pe.image_base + 0x1000, pe.image_base + pe.size_of_image

    def dword(va):
        return struct.unpack('<I', pe.read(va - pe.image_base, 4))[0]

    stale = [(int(f[0], 16), int(f[1], 16)) for f in read_tsv(stale_path)]
    stale_at = {va for va, _ in stale}
    notes = {int(f[0], 16): f[1] for f in read_tsv(notes_path)}

    def unrelocated(va, n):
        return not lo <= n < hi or va in stale_at

    def decide(va, why):
        return 'reviewed: ' + notes[va] if why == 'UNDECIDED' and va in notes else why

    rows = []
    for va, v in stale:
        neighbours = [(va + d, dword(va + d)) for d in (-4, 4)]
        rows.append(('data', va, v, decide(va, verdict(v, neighbours, unrelocated, False))))
    for f in read_tsv(missing_path):
        va, v = int(f[0], 16), int(f[2], 16)
        rows.append(('code', va, v, decide(va, verdict(v, (), unrelocated, True))))
    # Addresses and verdicts only: the values are bytes of the original exe.
    with open(out_path, 'w') as out:
        out.write('where\taddress\tverdict\n')
        for where, va, v, why in rows:
            out.write('%s\t%08x\t%s\n' % (where, va, why))
    counts = collections.Counter((where, why.split(':')[0]) for where, _, _, why in rows)
    for (where, why), n in sorted(counts.items()):
        print('%s\t%s\t%d' % (where, why, n))
    return 1 if any(why == 'UNDECIDED' for *_, why in rows) else 0


if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:]))
