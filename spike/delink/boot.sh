#!/bin/bash
# Boots an exe in the spike's own Wine prefix and screenshots the game window.
#
#   spike/delink/boot.sh <exe> <label> [seconds...]
#
# The prefix (work/delink/wine/prefix) is made fresh with the oracle's Wine and settings
# (oracle/wine-env.sh; fullscreen game in an 800x600 virtual desktop, as oracle/BASELINE.md
# records) and holds a clone of the 2.5.5.2 game folder. No CD key is ever imported into it, so
# the furthest any exe can get is the CD-key dialog; the oracle's own prefix is never touched.
# The exe is copied into the game folder as <label>.exe and run from there.
# At each sample time the oracle's input driver (oracle/driver, built by its build.sh) lists the
# top-level windows Wine has, which works while the Mac's screen is locked; the screenshot
# (oracle/shot.sh) needs an unlocked screen.
# Output: work/delink/boot/<label>/ with wine.log, windows_<n>s.txt, shot_<n>s.png, result.txt.
set -euo pipefail
source "$(dirname "$0")/env.sh"
EXE_IN="$1"; LABEL="$2"; shift 2
SHOTS=(15 30 60)
[ $# -gt 0 ] && SHOTS=("$@")

export WINEPREFIX="$WORK/wine/prefix"
export SFCOP_ROOT
source "$REPO/oracle/wine-env.sh"
export WINEDEBUG="${BOOT_WINEDEBUG:-fixme-all}"

if [ ! -f "$WINEPREFIX/system.reg" ]; then
	mkdir -p "$WINEPREFIX"
	wineboot -i >/dev/null 2>&1
	wineserver -w
	wine reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d Default /f >/dev/null 2>&1
	wine reg add 'HKCU\Software\Wine\Explorer\Desktops' /v Default /d 800x600 /f >/dev/null 2>&1
	wineserver -w
fi
if [ ! -f "$GAME_DIR/StarFleetOP.exe" ]; then
	expected=$(grep 'installed game/Starfleet Command Orion Pirates/StarFleetOP.exe$' "$HASHES" | cut -d' ' -f1 | head -1)
	echo "$expected  $EXE" | shasum -a 256 -c - >/dev/null
	mkdir -p "$(dirname "$GAME_DIR")"
	cp -Rc "$GAME" "$GAME_DIR"      # APFS clone: no extra space for the 790 MB folder
	chmod -R u+w "$GAME_DIR"
	perl -pi -e 's/^windowed=1/windowed=0/' "$GAME_DIR/sfc.ini"
fi

dir="$WORK/boot/$LABEL"
rm -rf "$dir"; mkdir -p "$dir"
rm -f "$GAME_DIR/$LABEL.exe"; cp "$EXE_IN" "$GAME_DIR/$LABEL.exe"; chmod u+w "$GAME_DIR/$LABEL.exe"
DRIVER="${ORACLE_INPUT:-$SFCOP_ROOT/work/oracle/build/out/oracle_input.exe}"
[ -f "$DRIVER" ] || sh "$REPO/oracle/driver/build.sh"
cp "$DRIVER" "$GAME_DIR/oracle_input.exe"
printf 'list\r\n' > "$GAME_DIR/list.steps"
shasum -a 256 "$EXE_IN" > "$dir/exe.sha256"
cd "$GAME_DIR"
wine "$LABEL.exe" > "$dir/wine.log" 2>&1 &
pid=$!
start=$(date +%s)
for t in "${SHOTS[@]}"; do
	while [ $(( $(date +%s) - start )) -lt "$t" ]; do sleep 1; done
	if kill -0 "$pid" 2>/dev/null; then
		wine oracle_input.exe list.steps > "$dir/windows_${t}s.txt" 2>/dev/null || true
		"$REPO/oracle/shot.sh" "$dir/shot_${t}s.png" 2>>"$dir/wine.log" || echo "${t}s: no screenshot" >> "$dir/result.txt"
	else
		echo "${t}s: exited" >> "$dir/result.txt"
	fi
done
if kill -0 "$pid" 2>/dev/null; then alive=yes; else alive=no; fi
wineserver -k || true
wait "$pid" 2>/dev/null || true
echo "alive after ${SHOTS[${#SHOTS[@]}-1]}s: $alive" >> "$dir/result.txt"
grep -c "Unhandled\|page fault" "$dir/wine.log" | sed 's/^/unhandled exceptions in wine.log: /' >> "$dir/result.txt" || true
cat "$dir/result.txt"
