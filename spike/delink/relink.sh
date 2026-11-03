#!/bin/bash
# Relinks the delinked object into a Windows exe with the VC6 SP4 link.exe from toolchain/.
#
#   spike/delink/relink.sh [out.exe] [extra objects...]
#
# Env: BASE (default 0x10000000; 0x400000 gives a same-base control build), OBJ (the delinked
# object, default work/delink/out/obj/sfcop.obj).
# Output next to the exe: <name>.map, <name>.link.log; the import libraries for the two DLLs
# with no VC6 library and the resource object are built under work/delink/out/.
set -euo pipefail
source "$(dirname "$0")/env.sh"

EXE_OUT="${1:-$OUT/exe/sfcop_relinked.exe}"
shift || true
EXTRA=("$@")
BASE="${BASE:-0x10000000}"
OBJ="${OBJ:-$OUT/obj/sfcop.obj}"
LINK="$TOOLCHAIN/bin/link"
LIB="$TOOLCHAIN/bin/lib"
# msvc-run derives the SFCOP root from its own path, which is wrong inside a worktree.
export SFCOP_ROOT
export MSVC_LIB="$VC6LIB/initial;$OUT/libs"

mkdir -p "$OUT/libs" "$(dirname "$EXE_OUT")"
cd "$OUT/libs"
for def in "$OUT"/imports/*.def; do
	stem=$(basename "$def" .def)
	[ "$OUT/libs/$stem.lib" -nt "$def" ] && continue
	"$LIB" /nologo /machine:ix86 "/def:Z:$def" "/out:$stem.lib" >/dev/null
done
python3 "$SPIKE/tools/rsrc2obj.py" "$EXE" "$OUT/rsrc.obj"

# The import libraries the original was linked with, in the original import order.
LIBS=$(awk -F'\t' 'NR>1 {print $5}' "$OUT/imports/imports.tsv" | awk '!seen[$0]++' | tr '\n' ' ')

name=$(basename "$EXE_OUT" .exe)
cd "$(dirname "$EXE_OUT")"
start=$(date +%s)
rc=0
# /FORCE is deliberately absent: an unresolved or duplicate symbol must fail the link.
"$LINK" /nologo /SUBSYSTEM:WINDOWS /ENTRY:WinMainCRTStartup /NODEFAULTLIB /FIXED /INCREMENTAL:NO \
	"/BASE:$BASE" "/OUT:$name.exe" "/MAP:$name.map" \
	"Z:$OBJ" "Z:$OUT/rsrc.obj" ${EXTRA[@]+"${EXTRA[@]/#/Z:}"} $LIBS > "$name.link.log" 2>&1 || rc=$?
end=$(date +%s)
sed 's/^/  /' "$name.link.log" | head -40
echo "link rc=$rc in $((end - start)) s -> $(dirname "$EXE_OUT")/$name.exe" \
	"$( [ -f "$name.exe" ] && stat -f %z "$name.exe") bytes" | tee -a "$name.link.log"
exit $rc
