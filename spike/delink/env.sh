# Shared settings for the P1-3 delink spike. Sourced by the other scripts (bash only).
# Override any of these from the environment.

SPIKE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SPIKE/../.." && pwd)"
# The main checkout (a worktree's git common dir is <main checkout>/.git); its parent holds
# work/ and tools/. A worktree has no reference/ of its own; it is reached through the main checkout.
MAIN_CHECKOUT="$(cd "$(git -C "$REPO" rev-parse --path-format=absolute --git-common-dir)/.." && pwd)"
SFCOP_ROOT="$(cd "$MAIN_CHECKOUT/.." && pwd)"
REFERENCE="${REFERENCE:-$MAIN_CHECKOUT/reference}"
GAME="$REFERENCE/installed game/Starfleet Command Orion Pirates"
EXE="$GAME/StarFleetOP.exe"
HASHES="$REPO/analysis/results/retail_hashes.sha256"

GHIDRA_HOME="${GHIDRA_HOME:-/opt/homebrew/Cellar/ghidra/12.2/libexec}"
HEADLESS="$GHIDRA_HOME/support/analyzeHeadless"
EXT_VERSION="v0.9.3"
EXT_ZIP="ghidra_12.2_PUBLIC_20261030_ghidra-delinker-extension.zip"
EXT_SHA256="40609a77a699559016773a341ae76fee2b58b3f4514c44e70e56023668939e35"

WORK="${WORK:-$SFCOP_ROOT/work/delink}"
SHARED_GZF="${SHARED_GZF:-$SFCOP_ROOT/work/ghidra/StarFleetOP_2552.gzf}"
SHARED_SETTINGS="$SFCOP_ROOT/work/ghidra/settings"
PROJECTS="$WORK/projects"
PROJECT="SFCOP_delink"
PROGRAM="StarFleetOP_2552"       # the program name inside the shared .gzf
LOGS="$WORK/logs"
OUT="$WORK/out"                  # objects, libs, relinked exe

TOOLCHAIN="$REPO/toolchain"
VC6LIB="$SFCOP_ROOT/tools/vc6-lib"

# At most 8 GB heap and 4 cores: several sessions share this machine.
export GHIDRA_HEADLESS_MAXMEM="${GHIDRA_HEADLESS_MAXMEM:-8G}"
# Isolated settings (holds the delinker extension), cache and temp, all on the external drive.
export GHIDRA_HEADLESS_JAVA_OPTIONS="-Dapplication.settingsdir=$WORK/settings -Dapplication.cachedir=$WORK/cache -Dapplication.tempdir=$WORK/tmp -Djava.io.tmpdir=$WORK/tmp -Dcpu.core.limit=${CORES:-4}"

mkdir -p "$PROJECTS" "$LOGS" "$OUT" "$WORK/settings" "$WORK/cache" "$WORK/tmp"

# run_headless <logname> <args...>: analyzeHeadless with wall time and peak RSS capture.
run_headless() {
	local name="$1"; shift
	echo "[$(date '+%H:%M:%S')] $name: analyzeHeadless $*" >&2
	rm -f "$LOGS/$name".*
	local rc=0
	nice -n 19 /usr/bin/time -l "$HEADLESS" "$@" -scriptPath "$SPIKE/ghidra_scripts" \
		-log "$LOGS/$name.log" -scriptlog "$LOGS/$name.script.log" \
		> "$LOGS/$name.out" 2> "$LOGS/$name.time" || rc=$?
	local secs rss
	secs=$(awk '/real/ {print $1; exit}' "$LOGS/$name.time")
	rss=$(awk '/maximum resident set size/ {printf "%.0f", $1/1048576; exit}' "$LOGS/$name.time")
	echo -e "$name\t${secs}s\t${rss}MB\trc=$rc" | tee -a "$LOGS/timings.tsv" >&2
	if grep -q "REPORT SCRIPT ERROR\|SCRIPT ERROR" "$LOGS/$name.out" 2>/dev/null; then
		echo "$name: script error, see $LOGS/$name.out" >&2; return 1
	fi
	return $rc
}
