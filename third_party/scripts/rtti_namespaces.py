#!/usr/bin/env python3
"""Count MSVC RTTI type descriptors (.?AV/.?AU names) per namespace in PE files, and grep marker strings.

With /GR every polymorphic class that is linked in has a type descriptor, so a namespace with
zero descriptors has no polymorphic classes in that binary.

usage: rtti_namespaces.py <out.md> <binary>...
"""
import re, sys
from collections import Counter
from pathlib import Path

WATCH = ["CryptoPP", "WONAPI", "WONCrypt", "std", "nSwitch"]
MARKERS = [b"CryptoPP", b"Crypto++", b"gamespy.com", b"gqueryreporting", b"AIL_", b"SmartHeap", b"MemPool",
           b"BugslayerUtil", b"SymInitialize"]

def main(out, *bins):
    rows = []
    for b in bins:
        data = Path(b).read_bytes()
        names = set(m.decode("latin-1") for m in re.findall(rb"\.\?A[UV][A-Za-z0-9_@?$]+@@", data))
        ns = Counter()
        for n in names:
            for w in WATCH:
                if f"@{w}@@" in n:
                    ns[w] += 1
        marks = {m.decode(): data.count(m) for m in MARKERS}
        rows.append((f"{Path(b).parent.name}/{Path(b).name}", len(names), ns, marks))
    lines = ["# RTTI namespaces and marker strings\n",
             "| Binary | RTTI names | " + " | ".join(WATCH) + " | " + " | ".join(m.decode() for m in MARKERS) + " |",
             "|---|---|" + "---|" * (len(WATCH) + len(MARKERS))]
    for name, total, ns, marks in rows:
        lines.append(f"| `{name}` | {total} | " + " | ".join(str(ns[w]) for w in WATCH) + " | "
                     + " | ".join(str(marks[m.decode()]) for m in MARKERS) + " |")
    Path(out).write_text("\n".join(lines) + "\n")
    print("\n".join(lines))

if __name__ == "__main__":
    main(*sys.argv[1:])
