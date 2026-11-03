#!/bin/sh
# Dry run of normalize_trace.sh and compare_traces.sh on synthetic traces (no Wine needed).
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
t=$(mktemp -d "${TMPDIR:-/tmp}/oracle-selftest.XXXXXX")
mk() {  # mk <dir> <speed-at-sample-1>
    mkdir -p "$1"
    printf '# oracle-trace v1\r\n# rng pin 1 crt_srand=0x%s lcg_seed=0x0088412C\r\n' "$3" > "$1/trace.raw.txt"
    printf 'S 0 c101 grid 6 20 raw 5 19 0 0 0 0 0 0 0 0 0 0 spd 0 dmg 0 tgt id-1 dist 64.5 st 2 \r\n' >> "$1/trace.raw.txt"
    printf 'S 1 c101 grid 6 20 raw 5 19 0 0 0 0 0 0 0 0 0 0 spd %s dmg 0 tgt c201 dist 64.25 st 2\r\n' "$2" >> "$1/trace.raw.txt"
    printf 'END 2 max_samples\r\n' >> "$1/trace.raw.txt"
    "$here/normalize_trace.sh" "$1/trace.raw.txt" > "$1/trace.norm.txt"
}
mk "$t/a" 4 007385DE; mk "$t/b" 4 00112233; mk "$t/c" 5 007385DE
echo "== a vs b (metadata differs only): expect identical"
"$here/compare_traces.sh" "$t/a" "$t/b"
echo "== a vs c (speed differs at sample 1): expect difference"
if "$here/compare_traces.sh" "$t/a" "$t/c"; then echo "selftest FAILED"; exit 1; fi
rm -rf "$t"
echo "selftest passed"
