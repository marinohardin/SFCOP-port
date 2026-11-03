#!/bin/sh
# Run a command inside the sfcop-lifter-py container: no network, 4 CPUs, 6 GB,
# only $LIFTER (work/lifter) mounted at /w, and this scripts/ dir read-only at /s.
#   sandbox.sh python -I pcrecomp/tools/cpp/rtti.py game/StarFleetOP.exe -o cat/rtti.json
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
LIFTER=${LIFTER:-$SFCOP_ROOT/work/lifter}
exec docker run --rm --network none --cpus 4 --memory 6g \
  -v "$LIFTER":/w -v "$HERE":/s:ro -w /w sfcop-lifter-py "$@"
