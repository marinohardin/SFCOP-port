#!/bin/sh
# Normalise an oracle trace for hashing: drop '#' metadata lines (they hold addresses and other
# run-specific facts), convert CRLF to LF, strip trailing blanks. Everything else is kept as
# written: the recorder already prints floats exactly (%.9g) and names ships by custom ID.
#
#   normalize_trace.sh trace.raw.txt > trace.norm.txt
set -eu
tr -d '\r' < "$1" | grep -v '^#' | sed 's/[[:space:]]*$//'
