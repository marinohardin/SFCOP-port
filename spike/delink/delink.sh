#!/bin/bash
# Delinks StarFleetOP.exe 2.5.5.2 into a COFF object with ghidra-delinker-extension v0.9.3.
#
#   spike/delink/delink.sh [steps]      steps: setup import prep export (default: all)
#
# setup   installs the extension (and the arm64 decompiler built by ghidra/) into an isolated
#         Ghidra settings dir under work/delink/
# import  copies the shared P0-3 program (.gzf) into a fresh project under work/delink/
#         (the shared project is never opened)
# prep    maps imports, then runs SfcDelinkPrep.java on the copy (logs in work/delink/out/prep)
# export  runs SfcDelinkExport.java: work/delink/out/obj/sfcop.obj plus its relocation table
#
# EXCLUDE_FN="addr ..." removes those functions from the object (see replace/).
set -euo pipefail
source "$(dirname "$0")/env.sh"

STEPS="${*:-setup import prep export}"
OBJ="${OBJ:-$OUT/obj/sfcop.obj}"

# Exported address ranges: everything the loader maps, minus what link.exe rebuilds
# (headers, import address table, import directory with its lookup tables and names) and the
# resources (tools/rsrc2obj.py).
RANGES=(
	+00401000-00794fff   # .text
	+00795000-00837fff   # .rdata
	-00795000-007955b7   #   import address table (data directory 12)
	-00835218-00837fff   #   import directory, lookup tables, hint/name table
	+00838000-00c82fff   # .data (initialized and bss; kept whole so its layout is unchanged)
	+00c83000-00c83fff   # .tls
)

step_setup() {
	echo "$EXT_SHA256  $WORK/downloads/$EXT_ZIP" | shasum -a 256 -c - >/dev/null 2>&1 || {
		mkdir -p "$WORK/downloads"
		curl -sSL -o "$WORK/downloads/$EXT_ZIP" \
			"https://github.com/boricj/ghidra-delinker-extension/releases/download/$EXT_VERSION/$EXT_ZIP"
		echo "$EXT_SHA256  $WORK/downloads/$EXT_ZIP" | shasum -a 256 -c -
	}
	local ext="$WORK/settings/$(whoami)-ghidra/ghidra_12.2_PUBLIC/Extensions"
	mkdir -p "$ext"
	rm -rf "$ext/ghidra-delinker-extension"
	(cd "$ext" && unzip -qo "$WORK/downloads/$EXT_ZIP")
	# The Homebrew Ghidra has no arm64 decompiler; reuse the one ghidra/build.sh built.
	local natives="$SHARED_SETTINGS/$(whoami)-ghidra/ghidra_12.2_PUBLIC/Extensions/SfcNatives"
	if [ -d "$natives" ]; then rm -rf "$ext/SfcNatives"; cp -R "$natives" "$ext/"; fi
	echo "extensions: $(ls "$ext" | tr '\n' ' ')" >&2
}

step_import() {
	grep "installed game/Starfleet Command Orion Pirates/StarFleetOP.exe" "$HASHES" |
		sed "s|installed game/|$REFERENCE/installed game/|" | shasum -a 256 -c - >/dev/null
	run_headless import "$PROJECTS" "$PROJECT" -import "$SHARED_GZF" -noanalysis -overwrite
}

step_prep() {
	python3 "$SPIKE/tools/imports.py" "$EXE" "$VC6LIB/initial" "$OUT/imports"
	run_headless prep "$PROJECTS" "$PROJECT" -process "$PROGRAM" -noanalysis \
		-postScript SfcDelinkPrep.java "$OUT/imports/imports.tsv" "$SPIKE/overrides.tsv" "$OUT/prep" \
		"$SPIKE/symbols.tsv"
	grep -E "^INFO .*SfcDelinkPrep.java> " "$LOGS/prep.out" | sed 's/.*SfcDelinkPrep.java> //' | tee "$OUT/prep/summary.txt"
}

step_export() {
	local args=("${RANGES[@]}")
	for fn in ${EXCLUDE_FN:-}; do args+=("-fn:$fn"); done
	mkdir -p "$(dirname "$OBJ")"
	run_headless "export-$(basename "$OBJ" .obj)" "$PROJECTS" "$PROJECT" -process "$PROGRAM" -noanalysis \
		-readOnly -postScript SfcDelinkExport.java "$OBJ" "$(dirname "$OBJ")" "${args[@]}"
	cat "$(dirname "$OBJ")/$(basename "$OBJ" .obj).summary.txt"
}

for s in $STEPS; do "step_$s"; done
