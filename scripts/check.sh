#!/bin/sh
# The one command that is the definition of done. CI runs the same steps, and an agent that
# says "done" has run this once at the end. Steps are added as the toolchain grows: the VC6
# compile-and-objdiff oracle and the replay oracle join once the harness (P1-5) exists.
# A step that is not applicable is removed, never left to pass vacuously.
#
# Usage: sh scripts/check.sh

set -eu

cd "$(dirname "$0")/.."

echo "== tests"
python3 -m pytest tests -q

echo "== hotspot list and CODEOWNERS agree"
python3 - <<'EOF'
import re, sys
pats = [l.strip() for l in open('.github/hotspots.txt') if l.strip() and not l.startswith('#')]
owners = [l.split()[0].lstrip('/').rstrip('/') for l in open('.github/CODEOWNERS') if l.strip() and not l.startswith('#')]
missing = [p for p in pats if p.replace('/**', '').rstrip('/') not in owners]
if missing:
    print("hotspots.txt patterns with no CODEOWNERS line:", missing); sys.exit(1)
print("ok:", len(pats), "hotspot patterns,", len(owners), "CODEOWNERS lines")
EOF

echo "check: ok"
