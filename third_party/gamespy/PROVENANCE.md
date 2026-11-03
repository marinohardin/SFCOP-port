# GameSpy SDK: provenance (nothing vendored)

## What the exe contains

`scripts/gamespy_components.py <exe> gamespy/components.md` reports the following components. Every one is
built into `StarFleetOP.exe` 2.5.5.2. There are no `GameSpy*.dll` imports.

| Component | Evidence (strings) | Approx. SDK generation |
|---|---|---|
| GP (Presence) client | `gpiInitialize`, `gpcm.gamespy.com`, `gpsp.gamespy.com`, `\lc\1`, `\newuser\`, `\pmatch\`, `\getprofile\\sesskey\`, disk cache `gp.info`, buddy `\auth\`/`\aack\` | Early GP, 2000-2001. Later SDK copies start their changelog at GP 1.08 (2002). |
| CD Key client (gcdkey) | `key.gamespy.com`, `\uon\\skey\%d`, `\disc\\pid\%d\cd\%s\ip\%d`, `CD Key or challenge too long`, `SOFTWARE\GameSpy\CDKeys` | 2000-2001 |
| Chat SDK (IRC to peerchat; possibly Peer on top) | `USER %s %s %s :%s`, `NICK %s`, `JOIN %s %s`, `UTM %s :%s`, `MODE %s +b *!*@%s`, `QUIT :Later!`. No `CRYPT`, so no encrypted peerchat. | Chat 1.0x, 1999-2001 |
| GHTTP | `GameSpyHTTP/1.0`, chunked transfer | 2001 |
| Patching / version check (pt) | `http://motd.gamespy.com/motd/vercheck.asp?productid=%d&versionuniqueid=%s&distid=%d`, `\newver\`, `\dlurl\`, `\fpfileid\` | 2001 |
| Server list v1 ("CEngine"/goa) | `master.gamespy.com`, `\list\%s\gamename\%s\final\`, `\gamename\%s\gamever\%s\location\0\validate\%s\final\\queryid\1.1\`, `\secure\` | v1, enctype 0 |
| Query & Reporting v1 | `\basic\ \info\ \rules\ \players\ \echo\%s \statechanged\ \heartbeat\%d\gamename\%s \validate\%s \queryid\%d.%d` | `gqueryreporting.c`, 2000 |
| Not present | No `qr2_`, SB2, natneg, gt2 or sake. | |

GameSpy Arcade integration: `gamespyarcade.com/launch/launch.asp?gamename=strfltcmd2`, plus the config keys
`GameSpy/Connection/Port` and `GameSpy/ServerInfo/GameName`. The game name is `strfltcmd2`.

The QR v1 strings sit right before `WONAPI::ThreadFunc unhandled exception` in `.rdata`. That suggests QR comes
in through WONAPI's `WONMisc/GameSpySupport` + `gqueryreporting.c`, which the WON archive also carries. The QR v1
strings in the archive's `gqueryreporting.c` (last changelog entry "11/9/00 - DDW") match the exe. This is an
inference from string order; it has not been confirmed with cross-references.

## Public source and licences

A Sonnet sub-agent surveyed the sources on 2026-11-02 (summary below). Nothing was downloaded except the WON
archive.

| Source | Components | Date | Licence / how it became public | Verdict |
|---|---|---|---|---|
| `madebr/WON`, `wonapi/WONMisc/gqueryreporting.c` (also `jsj2008/lithtech`, `bibendovsky/ltjs`) | QR v1 | 1999-2000 (header "Copyright 2000 GameSpy Industries") | Leaked WON tree, no licence | Reference only. Best match for QR. |
| `nival/Blitzkrieg` `Sources/src/GameSpy/` | cengine (server list v1), chat, peer, queryreporting, pinger | 1999-2001 | Nival's 2025 non-commercial, revocable licence. That licence covers Nival's IP and cannot relicense GameSpy code. | Reference only. Closest match for server list v1 and chat. |
| `nitrocaster/GameSpy` (also OpenXRay) | Full later SDK: GP, chat, peer, gcdkey, ghttp, pt, qr2, SB, natneg, gt2, sake | 2007 (GP 1.13, pt 2.06) | `license.txt` is BSD-3, "Copyright (c) 2011, IGN Entertainment". A third party uploaded it in 2014, and whether IGN published it is **unverified**. Glu said in 2014 that GameSpy would not be open-sourced. | Reference only. If the BSD text turns out to be genuine, it is the one copy that could be vendored, but it is 6 years too new to byte-match. |
| `openmoh/openmohaa` `code/gamespy/` | GP, chat, peer, gcdkey, ghttp, pt, qr2, SB | 2002-2007 | GPL-2 repo, but the GameSpy directory is third-party code with no licence of its own | Reference only. |
| `GameProgressive/UniSpySDK` | Full SDK | 2002-2010 | Its LICENSE says proprietary EA code, non-commercial use only | Reference only. |
| Server emulators: `openspy/openspy-core` (QR v1 in `code/qr/server/v1/`, SB v1, GP in `code/GP/server/commands/`, peerchat; no CD-key service) and `GameProgressive/UniSpyServer` (AGPL-3) | Server side | 2020s | openspy-core has no licence file; UniSpyServer is AGPL-3 | Protocol reference for our own server. |

The sub-agent found no clean-room or permissively licensed reimplementation of the GameSpy client SDK.

## Decision

**Nothing vendored.** No copy of the 2000-2001 GameSpy client code has a licence we can rely on.

GameSpy's services ended in 2014, so for the port:

- **Strip or stub** GP, CD key, pt and GHTTP vercheck. Login and patch checks go away, or move to our own
  services.
- **Replace** server list v1 and QR v1 with the project's own lobby (Phase 3). For old clients during the
  transition, point them at OpenSpy (`master.openspy.net` etc.), which still speaks QR v1 and SB v1.
- **Chat:** reimplement against plain IRC, or drop it.

For decompilation, name the functions with the help of Blitzkrieg (chat, cengine, qr) and the WON archive's
`gqueryreporting.c`. Use the 2007 SDK only to look up GP/gcdkey/pt function names, and expect the code to differ.
