#!/bin/sh
# Re-create the vendored trees from the fetched sources (run fetch_sources.sh first).
set -eu
work="${SFCOP_WORK:-$SFCOP_ROOT/work}"
here="$(cd "$(dirname "$0")/.." && pwd)"

# Crypto++ 3.1: library sources and project files; no test driver, validation suite or .dat vectors.
x="$work/third_party/dl/cryptopp31/x"
d="$here/cryptopp/src"
rm -rf "$d" && mkdir -p "$d"
for f in "$x"/*.cpp "$x"/*.h; do
  case "$(basename "$f")" in bench.*|test.cpp|validat*|validate.h) ;; *) cp "$f" "$d/" ;; esac
done
cp "$x/license.txt" "$x/readme.txt" "$x/cryptlib.dsp" "$x/cryptest.dsp" "$x/cryptest.dsw" "$x/Makefile" "$d/"

# WONAPI: headers only (reference; see wonapi/PROVENANCE.md), for the modules the exe links.
w="$(ls -d "$work"/src/WON/*network*/wonapi)"
d="$SFCOP_ROOT/work/src/wonapi-headers-ref"  # outside the repo: leaked source is never committed
rm -rf "$d" && mkdir -p "$d/WONMisc"
for f in WONAPI.h WONShared.h WONSharedCustom.h WONStatus.h WONTypes.h; do cp "$w/$f" "$d/"; done
for m in WONCommon WONSocket WONAuth WONCrypt WONDir WONServer; do mkdir -p "$d/$m"; cp "$w/$m"/*.h "$d/$m/"; done
for f in DetectFirewallOp GetMOTDOp HTTPGetOp HTTPEngine HTTPCache URL GetHTTPDocumentOp GameSpySupport CheckValidVersionOp; do
  cp "$w/WONMisc/$f.h" "$d/WONMisc/"
done
echo "vendored into $here"
