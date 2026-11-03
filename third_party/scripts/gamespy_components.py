#!/usr/bin/env python3
"""Identify GameSpy SDK components in a binary from characteristic strings.

usage: gamespy_components.py <binary> <out.md>
"""
import sys
from pathlib import Path

COMPONENTS = {
    "GP (Presence) client": [b"gpiInitialize", b"gpcm.gamespy.com", b"gpsp.gamespy.com", b"\\lc\\1",
                             b"\\newuser\\", b"\\getprofile\\\\sesskey\\", b"gp.info"],
    "CD Key client (gcdkey)": [b"key.gamespy.com", b"\\uoff\\\\skey\\%d", b"\\disc\\\\pid\\%d",
                               b"CD Key or challenge too long", b"SOFTWARE\\GameSpy\\CDKeys"],
    "Chat (IRC) / Peer": [b"USER %s %s %s :%s", b"UTM %s :%s", b"JOIN %s %s", b"QUIT :Later!",
                          b"MODE %s +b *!*@%s"],
    "GHTTP": [b"GameSpyHTTP/1.0", b"Transfer-Encoding: chunked"],
    "Patching / version check (pt)": [b"motd.gamespy.com/motd/vercheck.asp", b"\\newvername\\", b"\\fpfileid\\"],
    "Server list v1 (CEngine/goa)": [b"master.gamespy.com", b"\\list\\%s\\gamename\\%s\\final\\",
                                     b"\\validate\\%s\\final\\\\queryid\\1.1\\"],
    "Query & Reporting v1 (gqueryreporting)": [b"\\heartbeat\\%d\\gamename\\%s", b"\\statechanged\\",
                                               b"\\echo\\%s", b"\\queryid\\%d.%d"],
    "Query & Reporting 2 / SB2 (later SDK)": [b"qr2_", b"SBServer", b"sbserver", b"natneg"],
    "GameSpy Arcade launch / config": [b"gamespyarcade.com/launch", b"GameSpy/Connection/Port",
                                       b"GameSpy/ServerInfo/GameName"],
}

def main(binary, out):
    data = Path(binary).read_bytes()
    lines = [f"# GameSpy SDK components in `{Path(binary).name}`\n",
             "| Component | Markers found | Markers |", "|---|---|---|"]
    for comp, marks in COMPONENTS.items():
        found = [m for m in marks if m in data]
        lines.append(f"| {comp} | {len(found)}/{len(marks)} | "
                     + ", ".join(("`%s`" % m.decode()) + ("" if m in data else " (absent)") for m in marks) + " |")
    Path(out).write_text("\n".join(lines) + "\n")
    print("\n".join(lines))

if __name__ == "__main__":
    main(*sys.argv[1:])
