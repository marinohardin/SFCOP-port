#!/bin/sh
# Compare two oracle runs: equal hashes, or the first sample where they diverge and which fields.
#
#   oracle/compare_traces.sh <label-or-dir-A> <label-or-dir-B>
# Exit status: 0 identical, 1 different.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
dir() { if [ -d "$1" ]; then echo "$1"; else echo "$here/traces/$1"; fi; }
a=$(dir "$1")/trace.norm.txt
b=$(dir "$2")/trace.norm.txt

ha=$(shasum -a 256 "$a" | cut -d' ' -f1)
hb=$(shasum -a 256 "$b" | cut -d' ' -f1)
echo "A $ha $(wc -l < "$a") lines"
echo "B $hb $(wc -l < "$b") lines"
if [ "$ha" = "$hb" ]; then
    echo "identical"
    exit 0
fi

# First differing line, then a field-by-field view of it.
line=$(cmp "$a" "$b" 2>/dev/null | sed -n 's/.* line \([0-9]*\).*/\1/p')
[ -n "$line" ] || line=$(( $(wc -l < "$a") < $(wc -l < "$b") ? $(wc -l < "$a") + 1 : $(wc -l < "$b") + 1 ))
la=$(sed -n "${line}p" "$a")
lb=$(sed -n "${line}p" "$b")
echo "first difference at line $line (sample $(echo "$la" | cut -d' ' -f2))"
echo "A: $la"
echo "B: $lb"
printf '%s\n' "$la" | tr ' ' '\n' > "${TMPDIR:-/tmp}/oracle_a.$$"
printf '%s\n' "$lb" | tr ' ' '\n' > "${TMPDIR:-/tmp}/oracle_b.$$"
paste -d' ' "${TMPDIR:-/tmp}/oracle_a.$$" "${TMPDIR:-/tmp}/oracle_b.$$" | awk '{ if ($1 != $2) printf "  field %d: %s -> %s\n", NR, $1, $2 }'
rm -f "${TMPDIR:-/tmp}/oracle_a.$$" "${TMPDIR:-/tmp}/oracle_b.$$"
echo "differing lines: $(diff "$a" "$b" | grep -c '^[<>]' || true)"
exit 1
