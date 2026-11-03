#!/bin/bash
# Rerun the P1-7 DirectPlay inventory on a private copy of the shared Ghidra program (never the
# shared project). Raw output (TSVs, decompiled pseudocode) goes to $WORK/dump; nothing in the Ghidra
# project is saved (-readOnly). The small address-only tables are copied to analysis/netcode/results.
#   analysis/netcode/ghidra/run.sh
# About 4 minutes on the M1 Pro, 6 GB heap, 4 cores.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
RESULTS="$HERE/../results"
GHIDRA_HOME="${GHIDRA_HOME:-/opt/homebrew/Cellar/ghidra/12.2/libexec}"
SHARED="${SHARED:-$SFCOP_ROOT/work/ghidra}"
WORK="${WORK:-$SFCOP_ROOT/work/directplay}"
mkdir -p "$WORK/projects" "$WORK/logs" "$WORK/cache" "$WORK/tmp" "$WORK/dump" "$RESULTS"

# The arm64 decompiler built by ghidra/scripts/build_decompiler.sh lives in the shared settings dir;
# copy the extension into our own settings dir so the two never share state.
EXT="ghidra_12.2_PUBLIC/Extensions/SfcNatives"
if [ ! -x "$WORK/settings/$(whoami)-ghidra/$EXT/os/mac_arm_64/decompile" ]; then
	mkdir -p "$WORK/settings/$(whoami)-ghidra/$(dirname "$EXT")"
	cp -R "$SHARED/settings/$(whoami)-ghidra/$EXT" "$WORK/settings/$(whoami)-ghidra/$EXT"
fi
export GHIDRA_HEADLESS_MAXMEM="${GHIDRA_HEADLESS_MAXMEM:-6G}"
export GHIDRA_HEADLESS_JAVA_OPTIONS="-Dapplication.settingsdir=$WORK/settings -Dapplication.cachedir=$WORK/cache -Dapplication.tempdir=$WORK/tmp -Djava.io.tmpdir=$WORK/tmp -Dcpu.core.limit=4"
H="$GHIDRA_HOME/support/analyzeHeadless"

if [ ! -d "$WORK/projects/DP.rep" ]; then
	"$H" "$WORK/projects" DP -import "$SHARED/StarFleetOP_2552.gzf" -noanalysis > "$WORK/logs/import.out" 2>&1
fi

# 1. The wrapper region 0x0060F000-0x0061B800: functions, indirect calls, vtables, EH records,
#    references in from outside, decompiled pseudocode.
"$H" "$WORK/projects" DP -process StarFleetOP_2552 -noanalysis -readOnly \
	-scriptPath "$HERE" -postScript DpDump.java "$WORK/dump" > "$WORK/logs/dump.out" 2>&1
grep -E "DpDump:|SCRIPT ERROR" "$WORK/logs/dump.out"

# 2. Who reaches the wrapper: the static object at 0x00C3B478, its singleton pointer at 0x00C3B628,
#    and tNetControl's member pointer at +0x8D00.
"$H" "$WORK/projects" DP -process StarFleetOP_2552 -noanalysis -readOnly \
	-scriptPath "$HERE" -postScript DpUsers.java "$WORK/dump" 00C3B478-00C3B62C 8d00 > "$WORK/logs/users.out" 2>&1
grep -E "DpUsers:|SCRIPT ERROR" "$WORK/logs/users.out"

D="$WORK/dump"
cp "$D/eh.tsv" "$D/vtables.tsv" "$D/refchain.tsv" "$RESULTS/"
cut -f1-5 "$D/users_vcalls.tsv" > "$RESULTS/users_vcalls.tsv"
awk -F'\t' 'NR==1 || ($1>="006104d0" && $1<"0061b800" && $3 !~ /CALL 0x/)' "$D/vcalls.tsv" > "$RESULTS/region_vcalls.tsv"
echo "results in $RESULTS; raw dump and pseudocode in $D"
