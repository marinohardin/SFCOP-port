#!/usr/bin/env python3
"""Refuses a release package that carries anything from the original game or derived from it.

    python3 packaging/check_package.py PACKAGE HASH_LIST
    python3 packaging/check_package.py --kind KIND DIR HASH_LIST

PACKAGE is a finished asset (packaging/README.md): a .dmg (macOS: mounted read-only, its
SFCOP.app checked by packaging/macos/check_bundle.py), a .tar.gz (Linux) or a .zip (Windows),
unpacked into a temporary folder first, or the source archive (`*-source.tar.gz`), every file of
which is hashed (a source archive holds many files, so only the retail hashes and compiled
mission scripts are refused there). The temporary folder is made in SFCOP_CHECK_TMP, else beside
the asset, never in the system's temporary folder: an unpacked package is over 100 MB. With --kind, DIR is an unpacked tree: `linux` (the
tar.gz's folder), `appdir` (an AppImage's AppDir, or what `--appimage-extract` gives back) or
`windows` (the zip's folder).

A package may hold only the client's own files (LAYOUTS below). Every file is hashed: one whose
SHA-256 is in the hash list (the retail files) is refused, as is a compiled mission script, a
library that is not one of the client's (a translated mission module), any other file, any link
except the dmg's Applications link, and a missing file. The asset file itself must not be in the
list either. Exit 0 when clean, 1 with one line per refused file, 2 on bad arguments.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "macos"))
import check_bundle  # noqa: E402  (packaging/macos/check_bundle.py: the macOS bundle's rules)

# The files each unpacked tree must hold, and nothing else.
LAYOUTS = {
    "linux": {
        "sfcop",
        "bin/sfcop",
        "bin/retail_hashes.sha256",
        "lib/libSDL2-2.0.so.0",
        "lib/libSDL3.so.0",
        "share/applications/sfcop.desktop",
        "share/icons/hicolor/512x512/apps/sfcop.png",
        "README.txt",
    },
    "appdir": {
        "AppRun",
        "sfcop.desktop",
        "sfcop.png",
        ".DirIcon",
        "usr/bin/sfcop",
        "usr/bin/retail_hashes.sha256",
        "usr/lib/libSDL2-2.0.so.0",
        "usr/lib/libSDL3.so.0",
    },
    "windows": {
        "sfcop.exe",
        "SDL2.dll",
        "SDL3.dll",
        "retail_hashes.sha256",
        "README.txt",
    },
}

LIBRARY_SUFFIXES = (".dylib", ".dll", ".bundle")


def _is_library(name: str) -> bool:
    lowered = name.lower()
    return lowered.endswith(LIBRARY_SUFFIXES) or lowered.endswith(".so") or ".so." in lowered


def tree_refusals(root: str, hashes: set[str], expected: set[str]) -> list[str]:
    """One line per refused file of an unpacked tree, in path order; empty when it is clean."""
    found = []
    for dirpath, dirs, files in os.walk(root):
        dirs.sort()
        names = sorted(files + [d for d in dirs if os.path.islink(os.path.join(dirpath, d))])
        for name in names:
            path = os.path.join(dirpath, name)
            relative = os.path.relpath(path, root).replace(os.sep, "/")
            if os.path.islink(path):
                found.append(f"{relative}: a link; the package holds plain files only")
                continue
            if check_bundle.sha256_of(path) in hashes:
                found.append(f"{relative}: a file of the original game (its hash is in the list)")
            elif name.lower().endswith(".scr"):
                found.append(f"{relative}: a compiled mission script")
            elif relative not in expected and _is_library(name):
                found.append(f"{relative}: a library that is not the client's (a mission module?)")
            elif relative not in expected:
                found.append(f"{relative}: not one of the client's files")
    for relative in sorted(expected):
        if not os.path.isfile(os.path.join(root, relative)):
            found.append(f"{relative}: missing")
    return found


def _single_top(folder: str, what: str) -> tuple[str | None, list[str]]:
    entries = sorted(os.listdir(folder))
    if len(entries) == 1 and os.path.isdir(os.path.join(folder, entries[0])):
        return os.path.join(folder, entries[0]), []
    return None, [f"{what}: holds {entries}, not one folder"]


def _unpack_tar(asset: str, into: str, allow_links: bool = False) -> list[str]:
    """Unpacks plain files and folders only (and links that stay inside, for the source archive);
    anything else is refused before it is written."""
    found = []
    with tarfile.open(asset, "r:gz") as archive:
        for member in archive.getmembers():
            name = member.name
            if name.startswith("/") or ".." in name.split("/"):
                found.append(f"{name}: a path outside the package")
            elif allow_links and member.issym():
                continue  # tarfile's data filter refuses one that points outside
            elif not (member.isfile() or member.isdir()):
                found.append(f"{name}: a link or special file; the package holds plain files only")
        if found:
            return found
        archive.extractall(into, filter="data")
    return found


def _unpack_zip(asset: str, into: str) -> list[str]:
    found = []
    with zipfile.ZipFile(asset) as archive:
        for info in archive.infolist():
            name = info.filename
            mode = info.external_attr >> 16
            if name.startswith("/") or "\\" in name or ".." in name.split("/"):
                found.append(f"{name}: a path outside the package")
            elif mode and stat.S_ISLNK(mode):
                found.append(f"{name}: a link; the package holds plain files only")
        if found:
            return found
        archive.extractall(into)
    return found


def _hashed_refusals(path: str, label: str, hashes: set[str]) -> list[str]:
    """Refusals for every file under path (a file or a folder), named under label: a link, a file
    whose hash is in the list, a compiled mission script. Nothing else is refused."""
    if os.path.isdir(path) and not os.path.islink(path):
        files = []
        for dirpath, dirs, names in os.walk(path):
            dirs.sort()
            files += [os.path.join(dirpath, name) for name in sorted(names)]
            files += [os.path.join(dirpath, d) for d in dirs if os.path.islink(os.path.join(dirpath, d))]
    else:
        files = [path]
    found = []
    for full in sorted(files):
        relative = label if full == path else "/".join(
            part for part in (label, os.path.relpath(full, path).replace(os.sep, "/")) if part)
        if os.path.islink(full):
            found.append(f"{relative}: a link")
        elif check_bundle.sha256_of(full) in hashes:
            found.append(f"{relative}: a file of the original game (its hash is in the list)")
        elif full.lower().endswith(".scr"):
            found.append(f"{relative}: a compiled mission script")
    return found


def _dmg_refusals(asset: str, hashes: set[str], work: str) -> list[str]:
    mount = os.path.join(work, "mount")
    os.makedirs(mount)
    # hdiutil warns on every attach that diskutil is its successor; only a failure is shown.
    attach = subprocess.run(["hdiutil", "attach", "-readonly", "-nobrowse", "-noautoopen",
                             "-mountpoint", mount, asset], stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True)
    if attach.returncode != 0:
        return [f"{os.path.basename(asset)}: hdiutil could not mount it: {attach.stderr.strip()}"]
    try:
        found = []
        for name in sorted(os.listdir(mount)):
            path = os.path.join(mount, name)
            if name == "Applications" and os.path.islink(path):
                if os.readlink(path) != "/Applications":
                    found.append(f"Applications: links to {os.readlink(path)}, not /Applications")
            elif name == "SFCOP.app" and os.path.isdir(path) and not os.path.islink(path):
                found += [f"SFCOP.app/{line}" for line in check_bundle.refusals(path, hashes)]
            elif name in (".fseventsd", ".Trashes", ".DS_Store"):
                # macOS writes these on any HFS+ volume; allowed, but hashed like the rest.
                found += _hashed_refusals(path, name, hashes)
            else:
                found.append(f"{name}: not SFCOP.app or the Applications link")
        if not os.path.isdir(os.path.join(mount, "SFCOP.app")):
            found.append("SFCOP.app: missing")
        return found
    finally:
        subprocess.run(["hdiutil", "detach", mount, "-quiet"], check=False)


def asset_refusals(asset: str, hashes: set[str]) -> list[str]:
    """The refusals of a finished asset, by its extension."""
    if check_bundle.sha256_of(asset) in hashes:
        return [f"{os.path.basename(asset)}: a file of the original game (its hash is in the list)"]
    temp_root = os.environ.get("SFCOP_CHECK_TMP") or os.path.dirname(os.path.abspath(asset))
    os.makedirs(temp_root, exist_ok=True)
    work = tempfile.mkdtemp(prefix=".check-package-", dir=temp_root)
    try:
        return _asset_refusals(asset, hashes, work)
    except (tarfile.TarError, zipfile.BadZipFile, OSError) as error:
        return [f"{os.path.basename(asset)}: cannot be read as its kind of package ({error})"]
    finally:
        shutil.rmtree(work, ignore_errors=True)


def _asset_refusals(asset: str, hashes: set[str], work: str) -> list[str]:
    """The refusals of a finished asset by its extension, unpacked into work."""
    lowered = asset.lower()
    if lowered.endswith(".dmg"):
        return _dmg_refusals(asset, hashes, work)
    if lowered.endswith("-source.tar.gz"):
        found = _unpack_tar(asset, work, allow_links=True)
        # Links inside the archive are kept as links and not followed.
        return found or [line for line in _hashed_refusals(work, "", hashes)
                         if not line.endswith(": a link")]
    if lowered.endswith((".tar.gz", ".tgz")):
        found, kind = _unpack_tar(asset, work), "linux"
    elif lowered.endswith(".zip"):
        found, kind = _unpack_zip(asset, work), "windows"
    else:
        return [f"{os.path.basename(asset)}: not a .dmg, .tar.gz or .zip"]
    if found:
        return found
    top, found = _single_top(work, os.path.basename(asset))
    if top is None:
        return found
    return tree_refusals(top, hashes, LAYOUTS[kind])


def main(argv: list[str]) -> int:
    usage = __doc__.strip().splitlines()[2:4]
    if len(argv) == 5 and argv[1] == "--kind" and argv[2] in LAYOUTS:
        target, listing = argv[3], argv[4]
        if not os.path.isdir(target) or not os.path.isfile(listing):
            print("\n".join(usage), file=sys.stderr)
            return 2
        found = tree_refusals(target, check_bundle.read_hashes(listing), LAYOUTS[argv[2]])
    elif len(argv) == 3 and os.path.isfile(argv[1]) and os.path.isfile(argv[2]):
        target = argv[1]
        found = asset_refusals(target, check_bundle.read_hashes(argv[2]))
    else:
        print("\n".join(line.strip() for line in usage), file=sys.stderr)
        return 2
    for line in found:
        print(f"check_package: refused: {line}", file=sys.stderr)
    if found:
        return 1
    print(f"check_package: {target}: only the client's own files, none in the hash list")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
