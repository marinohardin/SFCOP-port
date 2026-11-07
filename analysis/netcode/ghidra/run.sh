#!/bin/bash
# Rerun the P1-7 DirectPlay inventory and the N1 Dynaverse walk on a private copy of the shared
# Ghidra program (never the shared project). Raw output (TSVs, decompiled pseudocode) goes to $WORK/dump; nothing in the Ghidra
# project is saved (-readOnly). The small address-only tables are copied to analysis/netcode/results.
#   analysis/netcode/ghidra/run.sh
# About 4 minutes on the M1 Pro, 6 GB heap, 4 cores, plus a few for the Dynaverse walk (step 3).
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

# 3. N1, DESIGN §12.6: which transport a Dynaverse mission's combat session uses. Forward walks from
#    the mission controller and the RunCampaign state, backward walks from tNetControl's host, join,
#    connect-by-address and enumerate entries (each with the stub that sets up its frame).
"$H" "$WORK/projects" DP -process StarFleetOP_2552 -noanalysis -readOnly \
	-scriptPath "$HERE" -postScript DvMission.java "$WORK/dump/dynaverse" \
	srcMissionCtrl=005EE000-005EF600 srcRunCampaign=00479AED-0047B2DC \
	srcLobbyUI=00450000-00479AED srcLobbyUI=0047B2DC-00480000 \
	srcMetaClient=005DC000-005EE000 srcMetaClient=005EF600-0060A000 \
	dstHost=004319C0-004319D0 dstJoin=00431C7F-00431C90 dstConnectByAddress=0043290E-00432920 \
	dstEnumSessions=00432CCE-00432CEA \
	netcontrol=0043104E-004343C0 wrapper=0060F000-0061B800 nswitch=00713000-0072B000 \
	> "$WORK/logs/dynaverse.out" 2>&1
grep -E "DvMission:|SCRIPT ERROR" "$WORK/logs/dynaverse.out"

D="$WORK/dump"
cp "$D/eh.tsv" "$D/vtables.tsv" "$D/refchain.tsv" "$RESULTS/"
cut -f1-5 "$D/users_vcalls.tsv" > "$RESULTS/users_vcalls.tsv"
awk -F'\t' 'NR==1 || ($1>="006104d0" && $1<"0061b800" && $3 !~ /CALL 0x/)' "$D/vcalls.tsv" > "$RESULTS/region_vcalls.tsv"
cp "$D/dynaverse/rpaths.tsv" "$RESULTS/dynaverse_rpaths.tsv"
awk -F'\t' 'NR==1 || ($1>="005ee000" && $1<"005ef600") || ($1>="00479aed" && $1<"0047b2dc")' "$D/dynaverse/paths.tsv" > "$RESULTS/dynaverse_paths.tsv"
awk -F'\t' 'NR==1 || ($1>="005ee000" && $1<"005ef600") || ($1>="00479aed" && $1<"0047b2dc")' "$D/dynaverse/edges.tsv" > "$RESULTS/dynaverse_edges.tsv"
echo "results in $RESULTS; raw dump and pseudocode in $D"
