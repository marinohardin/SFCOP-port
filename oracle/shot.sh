#!/bin/sh
# Screenshot of the game's own macOS window only (owner "wine", title "Starfleet Command"):
#
#   oracle/shot.sh out.png
#
# Never a screen region: a region also records whatever else is on the Mac's screen. Inside Wine,
# GDI capture (oracle_input's "shot") returns black because the game draws through DirectDraw/GL.
# Exit 2 if the window is not there.
set -eu
out=$1
id=$(osascript -l JavaScript -e '
ObjC.import("CoreGraphics");
var list = ObjC.deepUnwrap(ObjC.castRefToObject($.CGWindowListCopyWindowInfo($.kCGWindowListOptionAll, 0)));
var found = "";
list.forEach(function (w) {
    if (!found && w.kCGWindowOwnerName == "wine" && w.kCGWindowName == "Starfleet Command") found = String(w.kCGWindowNumber);
});
found;')
[ -n "$id" ] || { echo "shot.sh: no Starfleet Command window" >&2; exit 2; }
screencapture -x -o -l"$id" "$out"
