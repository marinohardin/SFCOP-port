#!/bin/sh
# Fails if any file changed on a branch matches a pattern in .github/hotspots.txt.
#
# Usage: checkHotspots.sh <base> <head>
#
# Compares <head> with the point where it left <base>, so changes made on the base branch
# since then are not counted against the pull request.

set -eu

if [ "$#" -ne 2 ]; then
  echo "Usage: $0 <base> <head>" >&2
  exit 2
fi

base=$1
head=$2
hotspots_file="$(dirname "$0")/../hotspots.txt"

# Turn each pattern into a git pathspec, collected in the positional parameters.
set --
while IFS= read -r line || [ -n "$line" ]; do
  case "$line" in
    '' | '#'*) continue ;;
  esac
  set -- "$@" ":(glob)$line"
done <"$hotspots_file"

# --no-renames lists both the old and the new path of a moved file.
changed=$(git diff --name-only --no-renames "$base...$head" -- "$@")

if [ -n "$changed" ]; then
  echo "This pull request changes hotspot files, which only the integrator changes:"
  echo
  echo "$changed" | sed 's/^/    /'
  echo
  echo "Describe the change you need and why (docs/PLAN.md, 'Contract changes') instead of making it here."
  echo "A pull request that is itself the contract change carries the contract-change label."
  exit 1
fi

echo "No hotspot files changed."
