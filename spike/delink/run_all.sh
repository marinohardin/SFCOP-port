#!/bin/bash
# Reproduces everything REPORT.md quotes and refreshes results/.
#
#   spike/delink/run_all.sh
#
# Delinks, relinks three builds (base 0x10000000, the same-base control, IsWindows98 replaced),
# validates each against the original, boots the original and the two rebuilt exes under Wine
# with a registry trace, and compares the traces. About 15 minutes, most of it the boots.
# Env: RUNS (boots per exe, default 3); BUILD=0 or BOOTS=0 skip those parts, so
# BUILD=0 BOOTS=0 only collects results/ again from work/delink.
set -euo pipefail
source "$(dirname "$0")/env.sh"
RES="$SPIKE/results"
BUILD="${BUILD:-1}"
BOOTS="${BOOTS:-1}"
RUNS="${RUNS:-3}"
HOLE='006f55a0-006f55bc=?IsWindows98@tOSVersion@@QAE_NXZ'

if [ "$BUILD" = 1 ]; then
	"$SPIKE/delink.sh"
	"$SPIKE/replace/build.sh"
	OBJ="$OUT/obj/sfcop_noiswin98.obj" EXCLUDE_FN=006f55a0 "$SPIKE/delink.sh" export
	"$SPIKE/relink.sh" "$OUT/exe/sfcop_relinked.exe"
	BASE=0x400000 "$SPIKE/relink.sh" "$OUT/exe/sfcop_samebase.exe"
	OBJ="$OUT/obj/sfcop_noiswin98.obj" "$SPIKE/relink.sh" "$OUT/exe/sfcop_replaced.exe" "$WORK/replace/IsWindows98_only.obj"
fi

mkdir -p "$RES"
for b in relinked samebase replaced; do
	relocs="$OUT/obj/sfcop.relocations.tsv"
	extra=()
	if [ "$b" = replaced ]; then
		relocs="$OUT/obj/sfcop_noiswin98.relocations.tsv"
		extra=(--exclude "$HOLE")
	fi
	python3 "$SPIKE/tools/validate.py" "$EXE" "$OUT/exe/sfcop_$b.exe" "$OUT/exe/sfcop_$b.map" "$relocs" \
		"$OUT/validate_$b" ${extra[@]+"${extra[@]}"}
	cp "$OUT/validate_$b/validate.txt" "$RES/validate_$b.txt"
	sed "s#$WORK#work/delink#g" "$OUT/exe/sfcop_$b.link.log" > "$RES/link_$b.txt"
done

# The replaced function against the original: equal except the dwords that hold fInfo.
python3 "$SPIKE/tools/compare_function.py" "$EXE" "$OUT/exe/sfcop_replaced.exe" "$OUT/exe/sfcop_replaced.map" \
	"${HOLE%%=*}" "${HOLE#*=}" "$RES/replaced_function.txt" '00c7f790=?fInfo@tOSVersion@@0U_OSVERSIONINFOA@@A'
# A verdict for every unrelocated value that looks like an image address.
python3 "$SPIKE/tools/candidates.py" "$EXE" "$OUT/validate_relinked/stale_candidates.tsv" \
	"$OUT/obj/sfcop.missing.tsv" "$SPIKE/candidate_notes.tsv" "$RES/candidates.tsv" > "$RES/candidates_summary.tsv"
cat "$RES/candidates_summary.tsv"

{ cat "$OUT/obj/sfcop.summary.txt"; echo "--- with tOSVersion::IsWindows98 excluded"
  cat "$OUT/obj/sfcop_noiswin98.summary.txt"; } > "$RES/export_summary.txt"
perl -pe 's/ \(GhidraScript\)\s*$/\n/' "$OUT/prep/summary.txt" > "$RES/prep_summary.txt"
awk -F'\t' 'NR>1 {split($4, a, " "); c[a[1]]++} END {for (k in c) printf "%s\t%d\n", k, c[k]}' \
	"$OUT/prep/prep_gaps.tsv" | sort > "$RES/prep_gap_actions.tsv"
awk -F'\t' 'NR>1 {c[$4]++} END {for (k in c) printf "%s\t%d\n", k, c[k]}' \
	"$OUT/prep/prep_refs.tsv" | sort > "$RES/prep_ref_actions.tsv"
grep -E '^(import|prep|export-sfcop|export-sfcop_noiswin98)[[:space:]]' "$LOGS/timings.tsv" | tail -4 > "$RES/timings.tsv"
awk -F'\t' 'NR>1 {n++; t += $3; s = ($1 < "00835218") ? ".rdata" : ".data/.tls"; c[s "\t" $3]++}
	END {printf "aligned data dwords with an image-address value and no relocation: %d, text-like: %d\n", n, t
	     for (k in c) {split(k, a, "\t"); printf "%s text_like=%s: %d\n", a[1], a[2] ? "True" : "False", c[k]}}' \
	"$OUT/validate_relinked/stale_candidates.tsv" > "$RES/stale_candidates_summary.txt"

export BOOT_WINEDEBUG="fixme-all,+reg"
labels=()
for i in $(seq 1 "$RUNS"); do
	for b in original relinked replaced; do
		exe="$OUT/exe/sfcop_$b.exe"
		[ "$b" = original ] && exe="$EXE"
		[ "$BOOTS" = 1 ] && "$SPIKE/boot.sh" "$exe" "run-$b-$i" 20 45 75 > "$LOGS/boot_run-$b-$i.log" 2>&1
		[ -f "$WORK/boot/run-$b-$i/result.txt" ] && labels+=("run-$b-$i")
	done
done
[ ${#labels[@]} -gt 0 ] || exit 0
up() { grep -qs 'class "SFC" "Starfleet Command"' "$WORK/boot/$1/windows_75s.txt"; }
# The registry traces are compared with the first original run that stayed up.
ref=""
for l in "${labels[@]}"; do
	case $l in run-original-*) up "$l" && { ref=$l; break; } ;; esac
done
printf 'run\tbuild\twinedebug\talive_at_last_sample\tsfc_window\tdeath\tregistry_trace\n' > "$RES/boot_runs.tsv"
rm -f "$RES"/regtrace_*.txt "$RES"/windows_*_75s.txt
bad=0
for l in "${labels[@]}"; do
	d="$WORK/boot/$l"; b=${l#run-}; b=${b%-*}
	alive=$(grep -o 'alive after [0-9]*s: [a-z]*' "$d/result.txt" | sed 's/alive after //')
	win=no
	up "$l" && win="yes (75s)"
	# The two ways Wine's start-up crash ends a run (oracle/BASELINE.md).
	death=$(grep -o -E 'Exception frame is not in stack limits|ignoring exception c0000005' "$d/wine.log" | head -1 || true)
	[ "$win" = no ] || cp "$d/windows_75s.txt" "$RES/windows_${l}_75s.txt"
	# Registry trace against the reference: "matches", "differs" (a run that stayed up and
	# differs fails run_all.sh), "differs, run died", or "-" for the original and when no
	# original run stayed up.
	trace=-
	if [ "$b" != original ] && [ -n "$ref" ]; then
		if python3 "$SPIKE/tools/regtrace.py" "$WORK/boot/$ref/wine.log" "$d/wine.log" "$ref" "$l" > "$d/regtrace.txt"; then
			trace="matches $ref"
			# Only the two-line summary of a matching pair is kept: a diff names local devices.
			cp "$d/regtrace.txt" "$RES/regtrace_$l.txt"
		elif [ "$win" = no ]; then
			trace="differs, run died"
		else
			trace="differs"
			echo "FLAG: $l stayed up but its registry trace differs from $ref (see $d/regtrace.txt)" >&2
			bad=1
		fi
	fi
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$l" "$b" "$BOOT_WINEDEBUG" "$alive" "$win" "${death:--}" "$trace" >> "$RES/boot_runs.tsv"
done
cat "$RES/boot_runs.tsv"
exit $bad
