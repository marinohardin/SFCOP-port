#!/bin/sh
# Packs the Windows release folder make_tree.sh wrote into OUT.zip, with the folder's name as its
# one top folder, then checks the archive with packaging/check_package.py (packaging/README.md).
#
#   sh packaging/windows/make_zip.sh TREE OUT.zip SOURCE_DIR
set -eu

if [ $# -ne 3 ]; then
    sed -n '5p' "$0" >&2
    exit 2
fi
tree=$1 out=$2 source=$3
here=$(cd "$(dirname "$0")" && pwd -P)
python=${SFCOP_PYTHON:-python3}
rm -f "$out"
"$python" -I "$here/../pack.py" zip "$tree" "$out"
if ! "$python" -I "$here/../check_package.py" "$out" "$source/analysis/results/retail_hashes.sha256"; then
    rm -f "$out"
    exit 1
fi
echo "make_zip: $out"
