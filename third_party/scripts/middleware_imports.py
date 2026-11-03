#!/usr/bin/env python3
"""List the exe's imports from middleware DLLs (Miles, SmartHeap, Bugslayer, DbgHelp).

usage: middleware_imports.py <StarFleetOP.exe> <out.md>   (needs pefile)
"""
import sys
from pathlib import Path
import pefile

WATCH = ("mss32.dll", "shw32.dll", "ha312w32.dll", "bugslayerutil.dll", "dbghelp.dll", "imagehlp.dll")

def main(exe, out):
    pe = pefile.PE(exe)
    lines = [f"# Middleware imports of `{Path(exe).name}`\n"]
    for e in pe.DIRECTORY_ENTRY_IMPORT:
        dll = e.dll.decode()
        if dll.lower() not in WATCH:
            continue
        names = [i.name.decode() if i.name else f"ordinal {i.ordinal}" for i in e.imports]
        lines.append(f"## {dll} ({len(names)})\n")
        lines.append(", ".join(f"`{n}`" for n in names) + "\n")
    other = sorted(e.dll.decode() for e in pe.DIRECTORY_ENTRY_IMPORT if e.dll.decode().lower() not in WATCH)
    lines.append("Other imported DLLs: " + ", ".join(other) + "\n")
    Path(out).write_text("\n".join(lines))
    print("\n".join(lines))

if __name__ == "__main__":
    main(*sys.argv[1:])
