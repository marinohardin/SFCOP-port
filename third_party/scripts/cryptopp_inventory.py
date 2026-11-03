#!/usr/bin/env python3
"""Pin the Crypto++ version of the SDK's cryptlibR.lib.

Lists the lib's object members and the CryptoPP classes they define (via `nm -A`), then checks
marker classes against the feature introductions in Crypto++'s History.txt.

usage: cryptopp_inventory.py <cryptlibR.lib> <out_dir> [cryptlib.dsp of a candidate release]
"""
import collections, re, subprocess, sys
from pathlib import Path

# (marker, regex on defined symbol names, version that introduced or removed it)
MARKERS = [
    ("namespace CryptoPP",            r"@CryptoPP@@",                 "3.0 added"),
    ("MD2",                           r"\bMD2@CryptoPP@@",            "3.0 added"),
    ("RC6",                           r"\bRC6\w*@CryptoPP@@",         "3.0 added"),
    ("MARS",                          r"\bMARS\w*@CryptoPP@@",        "3.0 added"),
    ("ARC4",                          r"\bARC4@CryptoPP@@",           "3.1 added"),
    ("Rijndael",                      r"\bRijndael\w*@CryptoPP@@",    "3.1 added"),
    ("Twofish",                       r"\bTwofish\w*@CryptoPP@@",     "3.1 added"),
    ("Serpent",                       r"\bSerpent\w*@CryptoPP@@",     "3.1 added"),
    ("DMAC",                          r"\bDMAC\b|\$DMAC@",            "3.1 added"),
    ("CBC_MAC",                       r"CBC_MAC",                     "3.1 added"),
    ("TripleDES (old name)",          r"\bTripleDES\w*@CryptoPP@@",   "renamed in 3.2"),
    ("DES_EDE (old name)",            r"\bDES_EDE(Encryption|Decryption)?@CryptoPP@@", "renamed in 3.2"),
    ("DES_EDE2 / DES_EDE3",           r"\bDES_EDE[23]\w*@CryptoPP@@", "3.2 added"),
    ("DES_XEX3 (DESX)",               r"DES_XEX3|DESX",               "3.2 added"),
    ("ECDSA",                         r"ECDSA",                       "3.2 added"),
    ("DefaultEncryptorWithMAC",       r"DefaultEncryptorWithMAC",     "3.2 added"),
    ("Skipjack",                      r"SKIPJACK|Skipjack",           "4.0 added"),
    ("CAST256",                       r"CAST256",                     "4.0 added"),
    ("Panama",                        r"Panama",                      "4.0 added"),
    ("SHA256/384/512",                r"SHA(256|384|512)@CryptoPP@@", "4.0 added"),
    ("XTR",                           r"XTR",                         "4.0 added"),
    ("Socket / WindowsPipe",          r"\bSocket@CryptoPP@@|WindowsPipe", "4.0 added"),
    ("MARC4",                         r"MARC4",                       "4.1 added"),
    ("MD4",                           r"\bMD4@CryptoPP@@",            "4.2 added"),
    ("ESIGN",                         r"ESIGN",                       "5.0 added"),
]

def main(lib, out, dsp=None):
    out = Path(out); out.mkdir(parents=True, exist_ok=True)
    text = subprocess.run(["nm", "-A", lib], capture_output=True, text=True).stdout
    members, defined = collections.defaultdict(set), set()
    for line in text.splitlines():
        m = re.match(r"^.*?\.lib:(.+?\.obj): +(?:[0-9a-f]+)? *([A-Za-z]) (\S+)$", line)
        if not m:
            continue
        obj, typ, sym = m.groups()
        obj = obj.split("\\")[-1]
        members[obj]
        if typ != "U":
            defined.add(sym)
            for cls in re.findall(r"(?:\?\$)?([A-Za-z_][A-Za-z0-9_]*)@CryptoPP@@", sym):
                members[obj].add(cls)
    paths = sorted({p for s in defined for p in re.findall(r"[A-Za-z]:\\[\w\\]+\.cpp", s)})
    with open(out / "cryptlib_members.tsv", "w") as f:
        f.write("object\tcryptopp_names\n")
        for obj in sorted(members):
            f.write(f"{obj}\t{' '.join(sorted(members[obj]))}\n")
    allsyms = "\n".join(sorted(defined))
    with open(out / "cryptlib_version_markers.md", "w") as f:
        f.write(f"# Crypto++ version markers in `{Path(lib).name}`\n\n")
        f.write(f"Objects: {len(members)}; defined symbols: {len(defined)}.\n\n")
        f.write("Source paths embedded in anonymous-namespace symbols:\n\n")
        for p in paths:
            f.write(f"- `{p}`\n")
        f.write("\n| Marker | Introduced/changed | Present |\n|---|---|---|\n")
        for name, rx, ver in MARKERS:
            hits = len(re.findall(rx, allsyms, re.M))
            f.write(f"| {name} | {ver} | {'yes (%d)' % hits if hits else 'no'} |\n")
        if dsp:
            srcs = set(re.findall(r"SOURCE=\.\\(\w+)\.cpp", Path(dsp).read_text(errors="replace")))
            objs = {o[:-4] for o in members}
            f.write(f"\nObjects vs `{dsp}`: lib {len(objs)}, dsp .cpp {len(srcs)}, "
                    f"only in lib {sorted(objs - srcs)}, only in dsp {sorted(srcs - objs)}.\n")
    print((out / "cryptlib_version_markers.md").read_text())

if __name__ == "__main__":
    main(*sys.argv[1:])
