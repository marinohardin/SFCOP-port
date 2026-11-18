#!/usr/bin/env python3
"""Refuses an SFCOP.app that carries anything from the original game or derived from it.

    python3 packaging/macos/check_bundle.py BUNDLE HASH_LIST
    python3 packaging/macos/check_bundle.py --self-test WORK_DIR

The bundle may hold only the files the client itself is made of (EXPECTED below). Every file in
it is hashed, and a file whose SHA-256 is in the hash list (the retail files, sha256sum's format)
is refused, as is a compiled mission script, any library that is not one of the client's, and any
file not on the list: a translated mission module, the build's writable copy of the install, or
anything else the build directory holds; so is a bundle missing one of its files. Exit 0 when
the bundle is clean, 1 with one line per refused file otherwise, 2 on bad arguments.
"""

import hashlib
import os
import subprocess
import sys

# Paths relative to the bundle; each must be there, except the signature, which appears once the
# bundle is signed. Whatever codesign writes under Contents/_CodeSignature/ is allowed (its
# hashes are still checked).
EXPECTED = {
    "Contents/Info.plist",
    "Contents/PkgInfo",
    "Contents/MacOS/sfcop-launch",
    "Contents/MacOS/sfcop",
    "Contents/Frameworks/libSDL2-2.0.0.dylib",
    "Contents/Frameworks/libSDL3.dylib",
    "Contents/Resources/retail_hashes.sha256",
    "Contents/Resources/install_manifest.tsv",
    "Contents/Resources/SFCOP.icns",
    "Contents/_CodeSignature/CodeResources",
}

SIGNATURE = "Contents/_CodeSignature/CodeResources"
SIGNATURE_DIR = "Contents/_CodeSignature/"

LIBRARY_SUFFIXES = (".dylib", ".so", ".dll", ".bundle")


def read_hashes(path):
    hashes = set()
    with open(path, encoding="utf-8") as listing:
        for line in listing:
            field = line.split(maxsplit=1)
            if field and len(field[0]) == 64:
                hashes.add(field[0].lower())
    return hashes


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for piece in iter(lambda: stream.read(1 << 20), b""):
            digest.update(piece)
    return digest.hexdigest()


SDL3_LIBRARY = "Contents/Frameworks/libSDL3.dylib"
MACHO_MAGICS = (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe")


def is_macho(path):
    with open(path, "rb") as stream:
        return stream.read(4) in MACHO_MAGICS


def linked_libraries(path):
    """The install names a Mach-O loads (otool -L)."""
    out = subprocess.run(["otool", "-L", path], capture_output=True, text=True).stdout
    return [line.split()[0] for line in out.splitlines()[1:] if line.startswith("\t")]


def defines_sdl3(path):
    """Whether the Mach-O defines SDL3 itself. sdl2-compat also defines SDL_* names (the SDL2 API),
    so the test is a function only SDL3 has: the GPU API."""
    out = subprocess.run(["nm", "-gU", path], capture_output=True, text=True).stdout
    return any(line.split()[-1:] == ["_SDL_CreateGPUDevice"] for line in out.splitlines())


def sdl_refusals(bundle):
    """One copy of SDL3 per bundle: the dylib is linked by something, and nothing else carries SDL."""
    found = []
    if not os.path.isfile(os.path.join(bundle, SDL3_LIBRARY)):
        return found
    linked = seen = False
    for root, _, files in os.walk(bundle):
        for name in sorted(files):
            path = os.path.join(root, name)
            relative = os.path.relpath(path, bundle).replace(os.sep, "/")
            if relative == SDL3_LIBRARY or os.path.islink(path) or not is_macho(path):
                continue
            seen = True
            if any(os.path.basename(dep).startswith("libSDL3") for dep in linked_libraries(path)):
                linked = True
            if defines_sdl3(path):
                found.append(f"{relative}: carries SDL3 itself while the bundle ships libSDL3.dylib (two copies)")
    if seen and not linked:
        found.append(f"{SDL3_LIBRARY}: nothing in the bundle links it")
    return found


def refusals(bundle, hashes):
    """One line per refused file, in path order; empty when the bundle is clean."""
    found = []
    for root, dirs, files in os.walk(bundle):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(root, name)
            relative = os.path.relpath(path, bundle).replace(os.sep, "/")
            if os.path.islink(path):
                found.append(f"{relative}: a link; the bundle holds plain files only")
                continue
            if sha256_of(path) in hashes:
                found.append(f"{relative}: a file of the original game (its hash is in the list)")
            elif name.lower().endswith(".scr"):
                found.append(f"{relative}: a compiled mission script")
            elif relative.startswith(SIGNATURE_DIR) and "/" not in relative[len(SIGNATURE_DIR):]:
                continue
            elif relative not in EXPECTED and name.lower().endswith(LIBRARY_SUFFIXES):
                found.append(f"{relative}: a library that is not the client's (a mission module?)")
            elif relative not in EXPECTED:
                found.append(f"{relative}: not one of the client's files")
    for relative in sorted(EXPECTED - {SIGNATURE}):
        if not os.path.isfile(os.path.join(bundle, relative)):
            found.append(f"{relative}: missing")
    return found + sdl_refusals(bundle)


def self_test(work):
    """The checker refuses each kind of file it is meant to, and passes a clean bundle."""
    import shutil

    bundle = os.path.join(work, "SFCOP.app")
    shutil.rmtree(work, ignore_errors=True)
    for relative in sorted(EXPECTED - {SIGNATURE}):
        path = os.path.join(bundle, relative)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as stream:
            stream.write(f"invented contents of {relative}\n")
    listing = os.path.join(work, "hashes.sha256")
    retail = b"invented bytes standing in for a retail file\n"
    with open(listing, "w", encoding="utf-8") as stream:
        stream.write(hashlib.sha256(retail).hexdigest() + "  Test Install/StarFleetOP.exe\n")
    ok = True

    def expect(what, wanted):
        nonlocal ok
        got = refusals(bundle, read_hashes(listing))
        if got != wanted:
            ok = False
            print(f"self-test: {what}: expected {wanted}, got {got}")

    expect("a clean bundle", [])
    signature = os.path.join(bundle, "Contents/_CodeSignature")
    os.makedirs(signature)
    for name in ("CodeResources", "CodeDirectory", "CodeSignature"):
        with open(os.path.join(signature, name), "w", encoding="utf-8") as stream:
            stream.write(f"invented {name}\n")
    expect("a signed bundle", [])
    planted = [
        ("Contents/Resources/gameroot/Assets/anything.bin", retail,
         "a file of the original game (its hash is in the list)"),
        ("Contents/Resources/Met_10Patrol.scr", b"invented\n", "a compiled mission script"),
        ("Contents/Frameworks/Met_10Patrol.dylib", b"invented\n",
         "a library that is not the client's (a mission module?)"),
        ("Contents/Resources/notes.txt", b"invented\n", "not one of the client's files"),
    ]
    for relative, data, reason in planted:
        path = os.path.join(bundle, relative)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as stream:
            stream.write(data)
        expect(relative, [f"{relative}: {reason}"])
        os.remove(path)
    launcher = os.path.join(bundle, "Contents/MacOS/sfcop-launch")
    os.rename(launcher, launcher + ".away")
    expect("a bundle without its launcher",
           ["Contents/MacOS/sfcop-launch.away: not one of the client's files",
            "Contents/MacOS/sfcop-launch: missing"])
    os.rename(launcher + ".away", launcher)
    # A retail file under one of the client's own names is still refused.
    renamed = os.path.join(bundle, "Contents/Resources/SFCOP.icns")
    with open(renamed, "wb") as stream:
        stream.write(retail)
    expect("a retail file under a client name",
           ["Contents/Resources/SFCOP.icns: a file of the original game (its hash is in the list)"])
    with open(renamed, "w", encoding="utf-8") as stream:
        stream.write("invented contents of Contents/Resources/SFCOP.icns\n")
    # The SDL check, with stand-ins for the Mach-O readers (the invented files are not Mach-O).
    global is_macho, linked_libraries, defines_sdl3
    saved = (is_macho, linked_libraries, defines_sdl3)
    try:
        is_macho = lambda path: True
        links = {"sfcop": ["@rpath/libSDL2-2.0.0.dylib"], "libSDL2-2.0.0.dylib": ["@rpath/libSDL3.dylib"]}
        syms = {}
        linked_libraries = lambda path: links.get(os.path.basename(path), [])
        defines_sdl3 = lambda path: syms.get(os.path.basename(path), False)
        expect("one SDL3, linked once", [])
        links["libSDL2-2.0.0.dylib"] = []
        expect("a libSDL3.dylib nothing links",
               ["Contents/Frameworks/libSDL3.dylib: nothing in the bundle links it"])
        links["libSDL2-2.0.0.dylib"] = ["@rpath/libSDL3.dylib"]
        syms["sfcop"] = True
        expect("a static SDL3 in the client beside the dylib",
               ["Contents/MacOS/sfcop: carries SDL3 itself while the bundle ships libSDL3.dylib (two copies)"])
    finally:
        is_macho, linked_libraries, defines_sdl3 = saved
    shutil.rmtree(work, ignore_errors=True)
    print("self-test: ok" if ok else "self-test: FAILED")
    return 0 if ok else 1


def main(argv):
    if len(argv) == 3 and argv[1] == "--self-test":
        return self_test(argv[2])
    if len(argv) != 3 or not os.path.isdir(argv[1]) or not os.path.isfile(argv[2]):
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    found = refusals(argv[1], read_hashes(argv[2]))
    for line in found:
        print(f"check_bundle: refused: {line}", file=sys.stderr)
    if found:
        return 1
    print(f"check_bundle: {argv[1]}: only the client's own files, none in the hash list")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
