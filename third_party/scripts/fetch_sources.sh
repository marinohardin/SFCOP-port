#!/bin/sh
# Fetch the third-party sources P0-6 evaluated into work/ (never into the repo), pinned by hash/commit.
# Then re-vendor with `vendor.sh`. Everything fetched is untrusted data: nothing here builds or runs it.
set -eu
work="${SFCOP_WORK:-$SFCOP_ROOT/work}"
dl="$work/third_party/dl"
src="$work/src"
mkdir -p "$dl/cryptopp31" "$src"

# Crypto++ 3.1 (4/27/1999). cryptopp.com still serves crypto31.zip but timed out from here on
# 2026-11-02; the Wayback copy of the same URL was used.
zip="$dl/cryptopp31/crypto31.zip"
want=0d4eb329cd59cac3c7f3a992ecab16b279749a902d4b10e3ebf4e949c4e14f5b
if [ ! -f "$zip" ]; then
  curl -fsSL --max-time 120 -o "$zip" "https://cryptopp.com/crypto31.zip" ||
  curl -fsSL --max-time 120 -o "$zip" "https://web.archive.org/web/20240714225437id_/https://cryptopp.com/crypto31.zip"
fi
echo "$want  $zip" | shasum -a 256 -c -
rm -rf "$dl/cryptopp31/x" && mkdir -p "$dl/cryptopp31/x" && unzip -q "$zip" -d "$dl/cryptopp31/x"
curl -fsSL --max-time 60 -o "$dl/History.txt" https://raw.githubusercontent.com/weidai11/cryptopp/master/History.txt

# WON archive (WONAPI + Titan API). Note the top-level directory name starts with a space.
if [ ! -d "$src/WON/.git" ]; then
  git clone -q https://github.com/madebr/WON.git "$src/WON"
fi
git -C "$src/WON" checkout -q 7774426f70d7fcf6c8b69d6336ed0438bb5159e6

# Homeworld source release, Titan headers only (sparse).
if [ ! -d "$src/homeworld/.git" ]; then
  git clone -q --filter=blob:none --sparse https://github.com/aheadley/homeworld.git "$src/homeworld"
  git -C "$src/homeworld" sparse-checkout set src/Titan
  git -C "$src/homeworld" sparse-checkout add /license.txt /README
fi
git -C "$src/homeworld" checkout -q c1e7f492eaea243af782376e8bd7b6ef147c5b8f
echo "fetched into $work"
