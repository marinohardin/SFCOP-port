#!/bin/sh
# Tests server/deploy/deploy.sh on a fresh Debian in a container (needs Docker; the Mac runs it,
# nice'd, and it is NOT part of scripts/check.sh because it downloads packages):
#
#   sh server/deploy/test-container.sh
#
# The image is the Debian 13 (trixie) base the lifter's Dockerfile pins (cmake/lifter/Dockerfile),
# by the same digest; the repository is mounted read-only. The last line is "container test: ok".
set -eu

image='python:3.13-slim@sha256:ae14d4ce8771606477a81446e7ef429edd6762305fc50c84507c1041fa8285d1'
root=$(cd "$(dirname "$0")/../.." && pwd)

command -v docker >/dev/null 2>&1 || { echo "skip: docker not found"; exit 0; }
exec nice -n 19 docker run --rm --cpus 2 -v "$root:/src:ro" "$image" sh /src/server/deploy/container-test-inner.sh
