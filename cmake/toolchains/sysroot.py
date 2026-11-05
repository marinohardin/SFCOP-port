"""Helpers for setup.sh: pin and repair an installed sysroot.

    python3 sysroot.py tree-hash <dir>   print the SHA-256 that pins the tree's content
    python3 sysroot.py relink <dir>      make absolute symlinks inside the tree relative

The tree hash covers every regular file (path and SHA-256 of its bytes) and every symlink (path
and target), sorted by path; directories, timestamps and modes are left out, so the same content
installed twice hashes the same. Files named `.sfcop-*` (setup.sh's own stamps) are skipped.
"""

from __future__ import annotations

import hashlib
import os
import sys


def tree_lines(root: str) -> list[str]:
    lines = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames.sort()
        names = list(filenames)
        # A symlink to a directory is listed in dirnames; os.walk does not follow it.
        names += [d for d in dirnames if os.path.islink(os.path.join(dirpath, d))]
        for name in sorted(names):
            if name.startswith(".sfcop-"):
                continue
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            if os.path.islink(path):
                lines.append(f"L {rel} {os.readlink(path)}")
            elif os.path.isfile(path):
                digest = hashlib.sha256()
                with open(path, "rb") as stream:
                    for block in iter(lambda: stream.read(1 << 20), b""):
                        digest.update(block)
                lines.append(f"F {rel} {digest.hexdigest()}")
    lines.sort()
    return lines


def tree_hash(root: str) -> str:
    digest = hashlib.sha256()
    for line in tree_lines(root):
        digest.update(line.encode("utf-8") + b"\n")
    return digest.hexdigest()


def relink(root: str) -> int:
    """Rewrite each symlink whose target is absolute (/lib/x.so) to the same file inside root."""
    count = 0
    for dirpath, dirnames, filenames in os.walk(root):
        for name in filenames + dirnames:
            path = os.path.join(dirpath, name)
            if not os.path.islink(path):
                continue
            target = os.readlink(path)
            if not target.startswith("/"):
                continue
            inside = os.path.join(root, target.lstrip("/"))
            os.unlink(path)
            os.symlink(os.path.relpath(inside, dirpath), path)
            count += 1
    return count


def main(argv: list[str]) -> int:
    if len(argv) != 3 or argv[1] not in ("tree-hash", "relink"):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    root = argv[2]
    if not os.path.isdir(root):
        print(f"sysroot.py: not a directory: {root}", file=sys.stderr)
        return 1
    if argv[1] == "tree-hash":
        print(tree_hash(root))
    else:
        print(f"relinked {relink(root)} absolute symlinks")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
