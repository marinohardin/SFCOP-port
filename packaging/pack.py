#!/usr/bin/env python3
"""Packs a release tree into a .tar.gz (Linux) or a .zip (Windows) the same way every time.

    python3 packaging/pack.py tar TREE OUT.tar.gz
    python3 packaging/pack.py zip TREE OUT.zip

The archive has one top folder, the tree's own name, and holds its regular files in path order,
with owner 0, a fixed time (SOURCE_DATE_EPOCH, else 1980-01-01) and mode 755 for an executable
file, 644 otherwise. A link or any other kind of file in the tree stops it. macOS's tar is not
used: it adds AppleDouble `._` files and extended attributes to an archive made on a Mac.
"""

from __future__ import annotations

import gzip
import io
import os
import sys
import tarfile
import zipfile

FIXED_TIME = 315532800  # 1980-01-01, the earliest time a zip can hold


def entries(tree: str) -> list[tuple[str, str, bool]]:
    """(path on disk, path in the archive, executable) for each file, in archive-path order."""
    top = os.path.basename(os.path.normpath(tree))
    found = []
    for dirpath, dirs, files in os.walk(tree):
        for name in dirs + files:
            path = os.path.join(dirpath, name)
            if os.path.islink(path) or not (os.path.isdir(path) or os.path.isfile(path)):
                raise SystemExit(f"pack: {path}: not a plain file or folder")
        for name in files:
            path = os.path.join(dirpath, name)
            relative = os.path.relpath(path, tree).replace(os.sep, "/")
            found.append((path, f"{top}/{relative}", os.access(path, os.X_OK)))
    return sorted(found, key=lambda entry: entry[1])


def write_tar(tree: str, out: str, when: int) -> None:
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.PAX_FORMAT) as archive:
        folders = set()
        for path, name, executable in entries(tree):
            parts = name.split("/")[:-1]
            for depth in range(1, len(parts) + 1):
                folder = "/".join(parts[:depth])
                if folder not in folders:
                    folders.add(folder)
                    info = tarfile.TarInfo(folder)
                    info.type, info.mode, info.mtime = tarfile.DIRTYPE, 0o755, when
                    archive.addfile(info)
            info = tarfile.TarInfo(name)
            info.size = os.path.getsize(path)
            info.mode = 0o755 if executable else 0o644
            info.mtime = when
            with open(path, "rb") as stream:
                archive.addfile(info, stream)
    with open(out, "wb") as raw, gzip.GzipFile(fileobj=raw, mode="wb", mtime=when,
                                               filename="", compresslevel=9) as packed:
        packed.write(buffer.getvalue())


def write_zip(tree: str, out: str, when: int) -> None:
    stamp = __import__("time").gmtime(max(when, FIXED_TIME))[:6]
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path, name, executable in entries(tree):
            info = zipfile.ZipInfo(name, date_time=stamp)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = ((0o100755 if executable else 0o100644) << 16)
            info.create_system = 3  # Unix, so the modes above are read
            with open(path, "rb") as stream:
                archive.writestr(info, stream.read())


def main(argv: list[str]) -> int:
    if len(argv) != 4 or argv[1] not in ("tar", "zip") or not os.path.isdir(argv[2]):
        print("\n".join(line.strip() for line in __doc__.strip().splitlines()[2:4]),
              file=sys.stderr)
        return 2
    when = int(os.environ.get("SOURCE_DATE_EPOCH", FIXED_TIME))
    (write_tar if argv[1] == "tar" else write_zip)(argv[2], argv[3], when)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
