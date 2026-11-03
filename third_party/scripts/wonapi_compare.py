#!/usr/bin/env python3
"""Compare the WONAPI classes and status strings in StarFleetOP.exe with a WONAPI source tree.

usage: wonapi_compare.py <StarFleetOP.exe> <wonapi_src_dir> <out.md>
"""
import re, sys
from pathlib import Path

def exe_strings(path, n=4):
    data = Path(path).read_bytes()
    return [m.group().decode("latin-1") for m in re.finditer(rb"[\x20-\x7e]{%d,}" % n, data)]

def main(exe, src, out):
    strs = exe_strings(exe)
    blob = "\n".join(strs)
    rtti = sorted({m for m in re.findall(r"\.\?A[UV]([A-Za-z0-9_]+)@WONAPI@@", blob)})
    status = sorted({s for s in re.findall(r"\bWS_[A-Za-z0-9_]+", blob)})
    files = [p for p in Path(src).rglob("*") if p.suffix.lower() in (".h", ".cpp", ".c")]
    text = {p: p.read_text(errors="replace") for p in files}
    alltext = "\n".join(text.values())
    def where(cls):
        rx = re.compile(r"\b(class|struct)\s+(WONAPI_API\s+)?%s\b" % re.escape(cls))
        hits = sorted({str(p.relative_to(src)) for p, t in text.items() if rx.search(t)})
        return hits
    lines = [f"# WONAPI in `{Path(exe).name}` vs `{src}`\n",
             f"Source files scanned: {len(files)}.\n",
             "## RTTI classes in namespace WONAPI\n", "| Class | Defined in source tree |", "|---|---|"]
    found = 0
    for c in rtti:
        h = where(c)
        found += bool(h)
        lines.append(f"| `{c}` | {', '.join('`%s`' % x for x in h[:3]) if h else '**not found**'} |")
    lines.insert(3, f"RTTI classes: {len(rtti)}; defined in tree: {found}.\n")
    # status strings: the exe embeds the enum names as text (WONStatus string table)
    sfound = [s for s in status if re.search(r"\b%s\b" % re.escape(s), alltext)]
    missing = [s for s in status if s not in sfound]
    lines += ["", "## `WS_*` status strings\n",
              f"In exe: {len(status)}; also present in tree: {len(sfound)}; missing from tree: {len(missing)}.\n"]
    if missing:
        lines.append("Missing: " + ", ".join(f"`{s}`" for s in missing) + "\n")
    # status names in the tree's WONStatus.h that the exe lacks (newer additions)
    tree_status = set(re.findall(r"\b(WS_[A-Za-z0-9_]+)\s*=", alltext))
    extra = sorted(tree_status - set(status))
    lines.append(f"`WS_*` enum values defined in tree but absent from exe: {len(extra)}.\n")
    if extra:
        lines.append(", ".join(f"`{s}`" for s in extra[:200]) + "\n")
    Path(out).write_text("\n".join(lines) + "\n")
    print("\n".join(lines[:8]))
    print("missing classes:", [c for c in rtti if not where(c)])
    print("status in exe", len(status), "found", len(sfound), "missing", missing[:40], "extra", len(extra))

if __name__ == "__main__":
    main(*sys.argv[1:])
