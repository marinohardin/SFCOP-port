#!/bin/sh
# The Linux package's sfcop (packaging/README.md): starts bin/sfcop with the package's own SDL
# libraries in lib/ ahead of the system's. The client finds its hash list beside bin/sfcop.
here=$(cd "$(dirname "$0")" && pwd -P)
LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LD_LIBRARY_PATH
exec "$here/bin/sfcop" "$@"
