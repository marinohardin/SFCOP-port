#!/bin/sh
# Run the replay oracle once against the game in the Wine prefix and print the trace hash.
#
#   oracle/run_oracle.sh [label]
#
# Needs: setup_prefix.sh done; work/oracle/build/out/{OracleSkirmish.scr,oracle_input.exe} built
# (scr/build.sh, driver/build.sh). Writes oracle/traces/<label>/:
#   trace.raw.txt   OracleTrace.txt as the game wrote it
#   trace.norm.txt  normalised trace (what is hashed)
#   trace.sha256    SHA-256 of trace.norm.txt
#   input.log       oracle_input.exe step log;  wine.log  game stdout/stderr
# Exit status: 0 with a complete trace, 1 on timeout or incomplete trace.
#
# Environment: ORACLE_TIMEOUT (seconds, default 1800), ORACLE_STEPS (default driver/skirmish.steps),
#   ORACLE_SHOTS (seconds, multiple of 5; 0 = off): oracle/shot.sh of the game window into
#   mac_<t>s.png, for calibration. Black unless the window is on the Mac's visible Space.
#   ORACLE_FIXED_STEP (milliseconds per frame, e.g. 16.667; unset = the wall clock, as before):
#   the game is started as `oracle_launch.exe --fixed-step MS StarFleetOP.exe`, on the fixed-step
#   clock of the native oracle (driver/README.md, "The fixed step under Wine"); needs
#   work/oracle/build/out/{oracle_launch.exe,oracle_clock.dll} from driver/build.sh, and refuses
#   to run without them, or with a step out of the launcher's range. run.txt then records the step.
#   ORACLE_START_WAIT (seconds between starting the game and the input driver, default 10; the
#   tests set 0).
set -eu

here=$(cd "$(dirname "$0")" && pwd)
. "$here/wine-env.sh"
out_dir=$SFCOP_ROOT/work/oracle/build/out
label=${1:-$(date +%Y%m%d-%H%M%S)}
dest=$here/traces/$label
timeout=${ORACLE_TIMEOUT:-1800}
steps=${ORACLE_STEPS:-$here/driver/skirmish.steps}
trace=$GAME_DIR/OracleTrace.txt

for f in "$out_dir/OracleSkirmish.scr" "$out_dir/oracle_input.exe" "$GAME_DIR/StarFleetOP.exe"; do
    [ -f "$f" ] || { echo "run_oracle.sh: missing $f (see oracle/README.md)" >&2; exit 2; }
done
step=${ORACLE_FIXED_STEP:-}
if [ -n "$step" ]; then
    # The launcher's range, checked here so that a bad step stops the run before it starts:
    # above 0 and at most 1000 ms, with at most three decimals.
    whole=${step%%.*}
    fraction=
    case $step in *.*) fraction=${step#*.} ;; esac
    us=x
    case $step in
        .* | *. | *.*.* | *[!0-9.]*) ;;
        *) case $fraction in
               ????*) ;;
               *) us=$(( $(printf '%s\n' "$whole" | sed 's/^0*//; s/^$/0/') * 1000 + \
                         $(printf '%s000\n' "$fraction" | cut -c1-3 | sed 's/^0*//; s/^$/0/') )) ;;
           esac ;;
    esac
    if [ "$us" = x ] || [ "$us" -lt 1 ] || [ "$us" -gt 1000000 ]; then
        echo "run_oracle.sh: ORACLE_FIXED_STEP is milliseconds above 0 and up to 1000, with at most three decimals, such as 16.667; not '$step'" >&2
        exit 2
    fi
    for f in "$out_dir/oracle_launch.exe" "$out_dir/oracle_clock.dll"; do
        [ -f "$f" ] || { echo "run_oracle.sh: ORACLE_FIXED_STEP is set but $f is missing; build it with oracle/driver/build.sh (the run would not be pinned)" >&2; exit 2; }
    done
fi
[ ! -e "$dest" ] || { echo "run_oracle.sh: $dest exists; pick another label" >&2; exit 2; }
mkdir -p "$dest"

# Install the script and the driver; start from a clean slate.
cp "$out_dir/OracleSkirmish.scr" "$GAME_DIR/Assets/Scripts/"
cp "$out_dir/oracle_input.exe" "$steps" "$GAME_DIR/"
if [ -n "$step" ]; then
    cp "$out_dir/oracle_launch.exe" "$out_dir/oracle_clock.dll" "$GAME_DIR/"
fi
rm -f "$trace" "$GAME_DIR"/oracle_shot_*.bmp
wineserver -k 2>/dev/null || true
wineserver -w 2>/dev/null || true

{
    echo "label $label"
    echo "date $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "wine $(wine --version)"
    echo "exe $(shasum -a 256 "$GAME_DIR/StarFleetOP.exe" | cut -d' ' -f1)"
    echo "scr $(shasum -a 256 "$out_dir/OracleSkirmish.scr" | cut -d' ' -f1)"
    echo "steps $(shasum -a 256 "$steps" | cut -d' ' -f1)"
    if [ -n "$step" ]; then
        echo "fixed_step $step"
        echo "launch $(shasum -a 256 "$out_dir/oracle_launch.exe" | cut -d' ' -f1)"
        echo "clock $(shasum -a 256 "$out_dir/oracle_clock.dll" | cut -d' ' -f1)"
    fi
} > "$dest/run.txt"

# StarFleetOP.exe directly: SFCOP.exe is only a boot stub (registry + intro movie).
# With ORACLE_FIXED_STEP, through the launcher, which puts the fixed-step clock in it first.
if [ -n "$step" ]; then
    ( cd "$GAME_DIR" && exec wine oracle_launch.exe --fixed-step "$step" StarFleetOP.exe ) > "$dest/wine.log" 2>&1 &
else
    ( cd "$GAME_DIR" && exec wine StarFleetOP.exe ) > "$dest/wine.log" 2>&1 &
fi
# Starting both at once races the wineserver and desktop start-up: the driver then saw no windows.
sleep "${ORACLE_START_WAIT:-10}"
( cd "$GAME_DIR" && exec wine oracle_input.exe "$(basename "$steps")" ) > "$dest/input.log" 2>&1 &

# Wait for the END record (the recorder's last line) or time out.
status=1
elapsed=0
while [ "$elapsed" -lt "$timeout" ]; do
    if [ -f "$trace" ] && grep -q '^END ' "$trace"; then
        status=0
        break
    fi
    sleep 5
    elapsed=$((elapsed + 5))
    if [ "${ORACLE_SHOTS:-0}" -gt 0 ] && [ $((elapsed % ORACLE_SHOTS)) -eq 0 ]; then
        "$here/shot.sh" "$dest/mac_${elapsed}s.png" 2>/dev/null || true
    fi
done

wineserver -k 2>/dev/null || true
wineserver -w 2>/dev/null || true

[ -f "$trace" ] && cp "$trace" "$dest/trace.raw.txt"
for shot in "$GAME_DIR"/oracle_shot_*.bmp; do
    [ -f "$shot" ] && mv "$shot" "$dest/"
done

if [ ! -f "$dest/trace.raw.txt" ]; then
    echo "run_oracle.sh: no trace after ${elapsed}s; see $dest/input.log and $dest/wine.log" >&2
    exit 1
fi

"$here/normalize_trace.sh" "$dest/trace.raw.txt" > "$dest/trace.norm.txt"
shasum -a 256 "$dest/trace.norm.txt" | cut -d' ' -f1 > "$dest/trace.sha256"
echo "$(cat "$dest/trace.sha256")  $label  $(grep '^END ' "$dest/trace.raw.txt" || echo 'END missing (timeout)')"
exit $status
