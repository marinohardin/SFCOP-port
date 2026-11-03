#!/bin/sh
# Start-up crash test: launch StarFleetOP.exe <runs> times, poll every 2 s, record how long each
# run stays alive (up to <seconds>). Settings come from the caller's environment (WINE_CPU_TOPOLOGY,
# etc.) or are applied to the prefix beforehand. Appends one line to baseline/survive.tsv.
#
#   survive.sh <label> [runs=3] [seconds=30]
# Env: SURVIVE_EXE (default "StarFleetOP.exe"; e.g. "start /wait /affinity 1 StarFleetOP.exe")
set -eu
here=$(cd "$(dirname "$0")" && pwd)
. "$here/../wine-env.sh"
label=$1
runs=${2:-3}
seconds=${3:-30}
exe=${SURVIVE_EXE:-StarFleetOP.exe}

results=""
cd "$GAME_DIR"
i=0
while [ "$i" -lt "$runs" ]; do
    i=$((i + 1))
    wineserver -k 2>/dev/null || true
    wineserver -w 2>/dev/null || true
    # shellcheck disable=SC2086
    (wine $exe > /dev/null 2>&1 &)
    # Time from when the game process exists (Wine start-up takes a few seconds).
    w=0
    while ! pgrep -f StarFleetOP.exe >/dev/null && [ "$w" -lt 20 ]; do sleep 1; w=$((w + 1)); done
    t=0
    r="${seconds}+"
    while [ "$t" -lt "$seconds" ]; do
        sleep 2
        t=$((t + 2))
        pgrep -f StarFleetOP.exe >/dev/null || { r=$t; break; }
    done
    results="$results $r"
done
wineserver -k 2>/dev/null || true
wineserver -w 2>/dev/null || true
printf '%s\t%s\t%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$label" "${results# }" >> "$here/survive.tsv"
echo "$label:$results"
