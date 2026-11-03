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

echo "vendored into $here"
