#!/usr/bin/env python3
"""Refuses a client binary that has a folder of the machine that built it compiled in.

    python3 packaging/check_paths.py BUILD_DIR BINARY...

The folders looked for are all read from BUILD_DIR's CMakeCache.txt at scan time, never written
here: the checkout, the build folder, the lift's inputs, the toolchain file, sysroot and compiler
folder (when they are on the checkout's disk), the tools folder SFCOP_TOOLS_DIR from the
environment, and every folder between one of these and the folder it shares with the checkout,
each also as its real path; and, wider, the checkout's top folder (the disk or home it is on) and
the builder's HOME.

Prints one count per binary and kind of folder, never the folder itself. Exit 0 when every count
is 0, 1 otherwise, 2 on bad arguments. SFCOP_ALLOW_BUILD_PATHS=1 turns a refusal into a warning
for a local test package; it is itself refused when GITHUB_ACTIONS is set.
"""

from __future__ import annotations

import os
import sys

CACHE_KEYS = {
    "CMAKE_HOME_DIRECTORY": "the checkout",
    "CMAKE_CACHEFILE_DIR": "the build folder",
    "SFCOP_INSTALL_DIR": "the install",
    "SFCOP_PCRECOMP_DIR": "the pcrecomp clone",
}

# Toolchain paths in the cache. They are looked for only where they share a folder below the top
# with the checkout (a tools folder on the same disk), never a system folder such as /usr/bin,
# whose name in a binary says nothing about the machine.
TOOLCHAIN_KEYS = {
    "CMAKE_TOOLCHAIN_FILE": "the toolchain file",
    "CMAKE_SYSROOT": "the sysroot",
    "CMAKE_C_COMPILER": "the compiler's folder",
    "CMAKE_CXX_COMPILER": "the compiler's folder",
}


def _shared_depth(path: str, checkout: str) -> str:
    """The folder path shares with checkout, or "" when they share only the root."""
    try:
        shared = os.path.commonpath([path, checkout])
    except ValueError:
        return ""
    return shared if shared.count("/") >= 2 else ""


def folders(build: str) -> dict[str, set[bytes]]:
    """Each kind of folder to look for, as the byte strings it could appear as."""
    found: dict[str, set[bytes]] = {"the build folder": {os.path.abspath(build).encode()}}
    toolchain: list[tuple[str, str]] = []
    with open(os.path.join(build, "CMakeCache.txt"), encoding="utf-8", errors="replace") as cache:
        for line in cache:
            key, _, value = line.rstrip("\n").partition("=")
            name = key.split(":", 1)[0]
            if name in CACHE_KEYS and value.strip():
                found.setdefault(CACHE_KEYS[name], set()).add(value.strip().rstrip("/").encode())
            elif name in TOOLCHAIN_KEYS and value.strip().startswith("/"):
                toolchain.append((TOOLCHAIN_KEYS[name], value.strip().rstrip("/")))
    checkout_values = [v.decode() for v in found.get("the checkout", set())]
    for kind, value in toolchain:
        if kind == "the compiler's folder":
            value = os.path.dirname(value)
        if any(_shared_depth(value, c) for c in checkout_values):
            found.setdefault(kind, set()).add(value.encode())
    tools = os.environ.get("SFCOP_TOOLS_DIR", "").rstrip("/")
    if tools.startswith("/") and tools.count("/") >= 2:
        found["the tools folder"] = {tools.encode()}
    for kind, forms in found.items():
        found[kind] = {form for value in forms
                       for form in (value, os.path.realpath(value.decode()).encode())
                       if len(form) > 1}
    # The folders above each lift input, down to the one it shares with the checkout.
    checkouts = [c.decode() for c in found.get("the checkout", set())]
    for kind in ("the install", "the pcrecomp clone", "the tools folder", "the sysroot",
                 "the compiler's folder"):
        for value in list(found.get(kind, set())):
            path = value.decode()
            for checkout in checkouts:
                try:
                    shared = os.path.commonpath([path, checkout])
                except ValueError:
                    continue
                parent = os.path.dirname(path)
                while len(parent) > len(shared) and parent.count("/") >= 2:
                    found.setdefault(f"a folder above {kind[4:]}", set()).add(parent.encode() + b"/")
                    parent = os.path.dirname(parent)
                if shared.count("/") >= 2:
                    found.setdefault(f"a folder above {kind[4:]}", set()).add(shared.encode() + b"/")
    tops = set()
    for checkout in found.get("the checkout", set()):
        parts = checkout.split(b"/")
        if len(parts) > 3 and parts[0] == b"":
            tops.add(b"/".join(parts[:3]) + b"/")
    if tops:
        found["the checkout's top folder"] = tops
    home = os.environ.get("HOME", "").rstrip("/")
    if home.count("/") >= 2:
        found["the home folder"] = {home.encode() + b"/"}
    return found


def counts(binary: str, wanted: dict[str, set[bytes]]) -> dict[str, int]:
    with open(binary, "rb") as stream:
        data = stream.read()
    return {kind: sum(data.count(form) for form in forms) for kind, forms in wanted.items()}


def main(argv: list[str]) -> int:
    if len(argv) < 3 or not os.path.isfile(os.path.join(argv[1], "CMakeCache.txt")):
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    wanted = folders(argv[1])
    bad = False
    for binary in argv[2:]:
        for kind, count in sorted(counts(binary, wanted).items()):
            print(f"check_paths: {os.path.basename(binary)}: {kind}: {count}")
            bad = bad or count > 0
    if not bad:
        return 0
    if os.environ.get("SFCOP_ALLOW_BUILD_PATHS") == "1":
        if os.environ.get("GITHUB_ACTIONS"):
            print("check_paths: refused: SFCOP_ALLOW_BUILD_PATHS is not allowed in GitHub Actions",
                  file=sys.stderr)
            return 1
        print("check_paths: warning: SFCOP_ALLOW_BUILD_PATHS=1: a package for local tests only",
              file=sys.stderr)
        return 0
    print("check_paths: refused: a build-machine folder is in the package's binaries",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
