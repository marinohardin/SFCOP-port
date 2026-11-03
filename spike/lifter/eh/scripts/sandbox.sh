#!/bin/sh
# Run a command inside the sfcop-lifter-py container (built by the spike's
# run_all.sh setup step): no network, 4 CPUs, 6 GB, only $LIFTER
# (work/lifter-eh) mounted at /w and this scripts/ dir read-only at /s.
#   sandbox.sh python -I /s/run_lift_eh.py --all
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
LIFTER=${LIFTER:-$SFCOP_ROOT/work/lifter-eh}
exec docker run --rm --network none --cpus 4 --memory 6g \
  -v "$LIFTER":/w -v "$HERE":/s:ro -w /w sfcop-lifter-py "$@"
