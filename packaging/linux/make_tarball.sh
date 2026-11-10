#!/bin/sh
# Packs the Linux release tree make_tree.sh wrote into OUT.tar.gz, with the tree's folder name as
# its one top folder, plain files only, owner and timestamps normalized; then checks the archive
# with packaging/check_package.py (packaging/README.md).
#
#   sh packaging/linux/make_tarball.sh TREE OUT.tar.gz SOURCE_DIR
set -eu

if [ $# -ne 3 ]; then
    sed -n '6p' "$0" >&2
    exit 2
fi
tree=$1 out=$2 source=$3
here=$(cd "$(dirname "$0")" && pwd -P)
python=${SFCOP_PYTHON:-python3}
rm -f "$out"
"$python" -I "$here/../pack.py" tar "$tree" "$out"
if ! "$python" -I "$here/../check_package.py" "$out" "$source/analysis/results/retail_hashes.sha256"; then
    rm -f "$out"
    exit 1
fi
echo "make_tarball: $out"
