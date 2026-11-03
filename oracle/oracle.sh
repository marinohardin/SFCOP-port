#!/bin/sh
# The whole oracle in one command: build the mission script and the input driver, install both into
# the Wine prefix, record two runs, compare them.
#
#   oracle/oracle.sh [label=ref]     -> oracle/traces/<label>-1/, oracle/traces/<label>-2/
#
# Needs setup_prefix.sh done, the CD key entered once (launch_game.sh) and a calibrated
# driver/skirmish.steps (README.md). Exit status: 0 when both runs finished and their hashes agree.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
label=${1:-ref}

"$here/scr/build.sh"
"$here/driver/build.sh"
"$here/run_oracle.sh" "$label-1"
"$here/run_oracle.sh" "$label-2"
"$here/compare_traces.sh" "$label-1" "$label-2"
