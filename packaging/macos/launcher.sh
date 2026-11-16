#!/bin/sh
# SFCOP.app/Contents/MacOS/sfcop-launch, the bundle's executable: starts the client beside it
# with the arguments it was given. Started from Finder there is no terminal, so the client's output is
# written to a file of its own for the run, logs/launch-YYYYmmdd-HHMMSS.log in the per-user data
# folder (--data DIR when given, else the platform's own, as src/app/DataDir.h finds it), or
# appended to $SFCOP_LAUNCH_LOG when that is set (no rotation then). The newest 10 run files are
# kept and older ones deleted. Started from a terminal the output stays there.
here=$(cd "$(dirname "$0")" && pwd -P)
if [ -t 2 ]; then
    exec "$here/sfcop" "$@"
fi
data="$HOME/Library/Application Support/SFCOP"
previous=
for argument in "$@"; do
    if [ "$previous" = --data ]; then
        data=$argument
    fi
    previous=$argument
done
keep=10
if [ -n "$SFCOP_LAUNCH_LOG" ]; then
    log=$SFCOP_LAUNCH_LOG
    mkdir -p "$(dirname "$log")" || exec "$here/sfcop" "$@"
else
    logs="$data/logs"
    mkdir -p "$logs" || exec "$here/sfcop" "$@"
    log="$logs/launch-$(date '+%Y%m%d-%H%M%S').log"
    [ -e "$log" ] && log="${log%.log}-$$.log"
    # The names sort by time, so all but the newest keep-1 of the earlier ones go, which with this
    # run's file leaves keep.
    old=$(cd "$logs" && ls -1 launch-*.log 2>/dev/null | sort -r | tail -n +"$keep")
    for name in $old; do
        rm -f "$logs/$name"
    done
fi
{
    echo "=== SFCOP started $(date '+%Y-%m-%d %H:%M:%S')"
} >>"$log"
exec "$here/sfcop" "$@" >>"$log" 2>&1
