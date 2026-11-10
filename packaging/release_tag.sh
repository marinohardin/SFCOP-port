#!/bin/sh
# The release workflow's tag and version (.github/workflows/release.yml):
#
#   sh packaging/release_tag.sh EVENT TAG VERSION_FILE
#
# EVENT is push (TAG is the pushed tag) or workflow_dispatch (TAG is the input; empty means v plus
# the VERSION file). The tag must be v plus the VERSION file's version, alone or with a `.` or `-`
# suffix for a trial draft (v0.1.0-alpha.test1), and only letters, digits, `.`, `_` and `-` after
# the v. Prints `TAG=<tag>` and `SFCOP_VERSION=<tag without v>` lines (for GITHUB_ENV); exit 1
# with the reason on stderr otherwise, 2 on bad arguments.
set -eu

if [ $# -ne 3 ] || [ ! -f "$3" ]; then
    sed -n '4p' "$0" >&2
    exit 2
fi
event=$1 tag=$2
file_version=$(tr -d ' \n' <"$3")
case $event in
    push) [ -n "$tag" ] || { echo "release_tag: a push without a tag" >&2; exit 1; } ;;
    workflow_dispatch) tag=${tag:-v$file_version} ;;
    *) echo "release_tag: EVENT is push or workflow_dispatch, not $event" >&2; exit 2 ;;
esac
if ! printf '%s\n' "$tag" | grep -Eq '^v[0-9A-Za-z._-]+$'; then
    echo "release_tag: $tag is not v followed by letters, digits, '.', '_' and '-' only" >&2
    exit 1
fi
case $tag in
    "v$file_version" | "v$file_version".?* | "v$file_version"-?*) ;;
    *) echo "release_tag: $tag does not start with v plus the VERSION file ($file_version)" >&2
       exit 1 ;;
esac
echo "TAG=$tag"
echo "SFCOP_VERSION=${tag#v}"
