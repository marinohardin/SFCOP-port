# GameSpy SDK components in `StarFleetOP.exe`

| Component | Markers found | Markers |
|---|---|---|
| GP (Presence) client | 7/7 | `gpiInitialize`, `gpcm.gamespy.com`, `gpsp.gamespy.com`, `\lc\1`, `\newuser\`, `\getprofile\\sesskey\`, `gp.info` |
| CD Key client (gcdkey) | 5/5 | `key.gamespy.com`, `\uoff\\skey\%d`, `\disc\\pid\%d`, `CD Key or challenge too long`, `SOFTWARE\GameSpy\CDKeys` |
| Chat (IRC) / Peer | 5/5 | `USER %s %s %s :%s`, `UTM %s :%s`, `JOIN %s %s`, `QUIT :Later!`, `MODE %s +b *!*@%s` |
| GHTTP | 2/2 | `GameSpyHTTP/1.0`, `Transfer-Encoding: chunked` |
| Patching / version check (pt) | 3/3 | `motd.gamespy.com/motd/vercheck.asp`, `\newvername\`, `\fpfileid\` |
| Server list v1 (CEngine/goa) | 3/3 | `master.gamespy.com`, `\list\%s\gamename\%s\final\`, `\validate\%s\final\\queryid\1.1\` |
| Query & Reporting v1 (gqueryreporting) | 4/4 | `\heartbeat\%d\gamename\%s`, `\statechanged\`, `\echo\%s`, `\queryid\%d.%d` |
| Query & Reporting 2 / SB2 (later SDK) | 0/4 | `qr2_` (absent), `SBServer` (absent), `sbserver` (absent), `natneg` (absent) |
| GameSpy Arcade launch / config | 3/3 | `gamespyarcade.com/launch`, `GameSpy/Connection/Port`, `GameSpy/ServerInfo/GameName` |
