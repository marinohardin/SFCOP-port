#!/bin/sh
# SFCOP.app/Contents/MacOS/sfcop-launch, the bundle's executable: starts the client beside it
# with the arguments it was given. Started from Finder there is no terminal, so the client's output is
# appended to logs/launch.log in the per-user data folder (--data DIR when given, else the
# platform's own, as src/app/DataDir.h finds it), or to $SFCOP_LAUNCH_LOG when that is set;
# started from a terminal it stays there.
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
log=${SFCOP_LAUNCH_LOG:-"$data/logs/launch.log"}
mkdir -p "$(dirname "$log")" || exec "$here/sfcop" "$@"
{
    echo
    echo "=== SFCOP started $(date '+%Y-%m-%d %H:%M:%S')"
} >>"$log"
exec "$here/sfcop" "$@" >>"$log" 2>&1
