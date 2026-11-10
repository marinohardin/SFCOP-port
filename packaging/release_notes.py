#!/usr/bin/env python3
"""Checks the release packages in a dist folder and writes the draft release's notes and asset list.

    python3 packaging/release_notes.py --dist DIR --version V --lift true|false --commit SHA
        --outcomes "macos-arm64=success linux-x64=failure ..." --hash-list LIST
        --notes OUT.md --assets OUT.txt [--reason "PRESET=why it was not built" ...]

Each expected asset (ASSETS below, or the source archive when the lift is off) that is in DIR is
checked again by packaging/check_package.py (the source archive's files are hashed one by one; an
AppImage was checked unpacked by make_appimage.sh when it was made, and here its own hash is
checked, because unpacking it needs a container of its architecture) and listed in OUT.txt, one path per line, when it passes. The
notes say what to download, give each asset's SHA-256, and name every package that is missing,
with the outcome of its build step, or the --reason given for it. Exit 0 unless a package was refused (1) or the arguments are
wrong (2); a missing package is reported in the notes, not as a failure.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_package  # noqa: E402

PRESETS = ["macos-arm64", "linux-x64", "linux-arm64", "windows-x64", "windows-arm64"]
# preset: (extension, required, who it is for)
ASSETS = {
    "macos-arm64": [(".dmg", True, "macOS 11 or later on Apple silicon")],
    "linux-x64": [(".tar.gz", True, "Linux x86-64, glibc 2.31 or later (SteamOS 3, Ubuntu 20.04+)"),
                  (".AppImage", False, "the same, as one file")],
    "linux-arm64": [(".tar.gz", True, "Linux arm64, glibc 2.36 or later (Debian 12+, Ubuntu 22.10+)"),
                    (".AppImage", False, "the same, as one file")],
    "windows-x64": [(".zip", True, "Windows 10 or later, x64")],
    "windows-arm64": [(".zip", True, "Windows 11 on Arm")],
}


def sha256(path: str) -> str:
    return check_package.check_bundle.sha256_of(path)


def parse_outcomes(text: str) -> dict[str, str]:
    found = {}
    for item in text.split():
        preset, _, outcome = item.partition("=")
        found[preset] = outcome or "not run"
    return found


def collect(dist: str, version: str, lift: bool, outcomes: dict[str, str], hashes: set[str],
            reasons: dict[str, str] | None = None):
    """(rows of (asset name, sha256, who), missing lines, refused lines, asset paths)."""
    rows, missing, refused, paths = [], [], [], []
    if not lift:
        wanted = [(f"SFCOP-{version}-source.tar.gz", True, "the source; build it yourself", None)]
    else:
        wanted = [(f"SFCOP-{version}-{preset}{ext}", required, who, preset)
                  for preset in PRESETS for ext, required, who in ASSETS[preset]]
    for name, required, who, preset in wanted:
        path = os.path.join(dist, name)
        if not os.path.isfile(path):
            if required and preset and (reasons or {}).get(preset):
                missing.append(f"{name}: not built: {reasons[preset]}")
            elif required:
                outcome = outcomes.get(preset, "not run") if preset else "not run"
                missing.append(f"{name}: not built (the {preset or 'source'} step: {outcome})")
            elif preset and outcomes.get(preset) == "success":
                missing.append(f"{name}: not built (optional; the tar.gz is the Linux package)")
            continue
        if name.endswith(".AppImage"):
            problems = [f"{name}: a file of the original game"] if sha256(path) in hashes else []
        else:
            problems = check_package.asset_refusals(path, hashes)
        if problems:
            refused += problems
            continue
        rows.append((name, sha256(path), who))
        paths.append(path)
    return rows, missing, refused, paths


def notes_text(version: str, commit: str, lift: bool, rows, missing) -> str:
    lines = [f"SFCOP {version}, built from {commit[:12]}. Alpha.", ""]
    lines += ["A native client for Star Trek Starfleet Command II: Orion Pirates 2.5.5.2. "
              "**It contains no file of the original game**: you need your own install, patched "
              "to 2.5.5.2. The README's \"Installing a release\" says what to download and how to "
              "start it the first time.", ""]
    if not lift:
        lines += ["This release is the source only; build it with `sh scripts/dist.sh`.", ""]
    lines += ["| Download | For | SHA-256 |", "| --- | --- | --- |"]
    lines += [f"| `{name}` | {who} | `{digest}` |" for name, digest, who in rows]
    lines.append("")
    if lift:
        lines += ["The macOS app is not signed. The first time, open it once; when macOS refuses, "
                  "open System Settings > Privacy & Security and choose Open Anyway (on macOS 14 "
                  "and older, right-click the app and choose Open). Missions need translated "
                  "mission modules, which no package carries; the README says how to make them "
                  "from your install.", ""]
    if missing:
        lines += ["Not in this release:", ""] + [f"- {line}" for line in missing] + [""]
    lines.append("Every package was checked: it holds only the client's own files, and none of "
                 "them is a file of the original game.")
    return "\n".join(lines) + "\n"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    for flag in ("--dist", "--version", "--lift", "--commit", "--outcomes", "--hash-list",
                 "--notes", "--assets"):
        parser.add_argument(flag, required=True)
    parser.add_argument("--reason", action="append", default=[])
    args = parser.parse_args(argv[1:])
    if args.lift not in ("true", "false") or not os.path.isdir(args.dist):
        print("release_notes: --lift is true or false, and --dist a folder", file=sys.stderr)
        return 2
    lift = args.lift == "true"
    hashes = check_package.check_bundle.read_hashes(args.hash_list)
    reasons = {}
    for item in args.reason:
        preset, _, why = item.partition("=")
        if why.strip():
            reasons[preset] = why.strip()
    rows, missing, refused, paths = collect(args.dist, args.version, lift,
                                            parse_outcomes(args.outcomes), hashes, reasons)
    for line in refused:
        print(f"release_notes: refused: {line}", file=sys.stderr)
    for line in missing:
        print(f"release_notes: {line}")
    for name, digest, _ in rows:
        print(f"release_notes: checked: {name} {digest}")
    with open(args.notes, "w", encoding="utf-8") as stream:
        stream.write(notes_text(args.version, args.commit, lift, rows, missing))
    with open(args.assets, "w", encoding="utf-8") as stream:
        stream.write("".join(f"{path}\n" for path in paths))
    return 1 if refused else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
