#!/bin/sh
# SFCOP.app/Contents/MacOS/sfcop-launch, the bundle's executable: starts the client beside it
# with the arguments it was given. Started from Finder there is no terminal, so each run writes two
# files in logs/ of the per-user data folder (--data DIR when given, else the platform's own, as
# src/app/DataDir.h finds it, or $SFCOP_LOG_DIR), named by one stamp: launch-<stamp>.log, everything the client prints,
# and important-<stamp>.log, the few lines that matter after a crash (the client writes it, at the
# path in SFCOP_IMPORTANT_LOG; src/platform/Core/Log.h). The files of the newest 10 runs are kept, both
# of a run together, and older ones deleted; a launch.log from before is left alone. With
# $SFCOP_LAUNCH_LOG set the output is appended to that one file instead, with no important log and
# no pruning. Started from a terminal the output stays there.
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
    exec "$here/sfcop" "$@" >>"$log" 2>&1
fi
logs=${SFCOP_LOG_DIR:-"$data/logs"}
mkdir -p "$logs" || exec "$here/sfcop" "$@"
stamp=$(date '+%Y%m%d-%H%M%S')
[ -e "$logs/launch-$stamp.log" ] && stamp="$stamp-$$"
# The stamps sort by time. Of the runs before this one the newest keep-1 stay, which with this run
# leaves keep; both files of a run go together.
old=$(cd "$logs" && ls -1 launch-*.log important-*.log 2>/dev/null | sed -e 's/^launch-//' -e 's/^important-//' -e 's/\.log$//' | sort -ur | tail -n +"$keep")
for old_stamp in $old; do
    rm -f "$logs/launch-$old_stamp.log" "$logs/important-$old_stamp.log"
done
SFCOP_IMPORTANT_LOG="$logs/important-$stamp.log"
export SFCOP_IMPORTANT_LOG
exec "$here/sfcop" "$@" >>"$logs/launch-$stamp.log" 2>&1
