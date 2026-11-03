# Research report: Dynaverse II / WON / GameSpy

Sub-agent report, 2026-11-02.

## 1. WONAPI / WON SDK: open-sourced, leaked, reimplemented?

Never officially open-sourced; partial SDKs recovered from game releases; several reimplementations exist.

- WON was proprietary (Sierra SIGS -> WON -> flipside.com). https://en.wikipedia.org/wiki/World_Opponent_Network
- Homeworld 1 source (Relic RDN, 2003/2004) includes the WON/Titan client headers: `src/Titan/{EasySocket,auth,common,crypt,db,inc,msg}`. https://github.com/aheadley/homeworld
  - `crypt/`: `EGPrivateKey.h`, `EGPublicKey.h` (ElGamal), `BFSymmetricKey.h` (Blowfish).
  - `auth/`: `Auth1Certificate.h`, `Auth1PublicKeyBlock.h`, `AuthFamilyBuffer.h`.
  - `msg/`: `Auth, Comm, Dir, Event, Fact, Firewall, Routing`; `msg/Dir` has 34 headers (TMsg gen-1 and SMsgDirG2 gen-2) = the DirServer wire protocol.
  - This is the older "Titan" C++ API (1999), not the 2001 `WONAPI::` layer seen in SFC:OP, but the server message families are the same. Homeworld had `/noWon` and `/noAuth` debug flags.
- Recovered WON SDKs (community archive): `kin37ik/WON` (forks `madebr/WON`, `hereelabs/won`): "WONAPI and Titan API complete"; full WONAPI SDK from Dark Reign 2, partial from AVP2, header-only from Homeworld; plus docs. Their server code was never released. https://github.com/madebr/WON ; https://github.com/hereelabs/won
- Dark Reign 2 source (released 2011) is the known public source of a full WONAPI SDK (per the above README; unverified detail).
- Open-source WON server reimplementation (2026, active): FlashZ/won_oss_server + FlashZ/homeworld-classic-online: Python, AGPL-3.0, "WON-style Auth1, directory, routing, factory, and firewall services" for Homeworld 1.05 / Cataclysm; created 2026-03, pushed 2026-09-02. Clean-room from packet captures + Homeworld source; `docs/rebuilding-retail-won-with-ai.md`. Best starting point for an SFC:OP directory/auth emulator. https://github.com/FlashZ/won_oss_server
- Closed-source WON revivals: WON2 (Half-Life) https://won2.net/About-WON2 ; NeuWon/NuWON (SWAT3/HL; unverified); NeoEE (Empire Earth). Tribes 2: TribesNext.
- No "OpenWON" protocol-doc project found by that name.

## 2. GameSpy emulators usable by a 2001 GameSpy SDK client

- OpenSpy (openspy/openspy-core, C++): QR v1 (27900) and v2; Server Browsing v1 enctype 0/2 on 28900, v2 on 28910; GP presence 29900, search 29901; GStats 29920; Peerchat IRC 6667; NatNeg. Repo pushed 2026-01-04; org active through Jul 2026. Live DNS: master.openspy.net, gpcm.openspy.net, peerchat.openspy.net. https://github.com/openspy/openspy-core
- UniSpyServer (GameProgressive, Python, AGPL-3): pushed 2026-10-27. https://github.com/GameProgressive/UniSpyServer
- PRMasterServer (realitymod, C#): BF2-only; last push 2016; unsuitable. https://github.com/realitymod/PRMasterServer
- 333networks: GameSpy v0 master only; no GP/peerchat. https://333networks.com/masterserver
- SFC-specific: Revan67/SFC3Dynaverse (Python, SQLite, GPL-3; created 2026-05, pushed 2026-09-12) reimplements for SFC3 GP 29900/29901, master 28900, UDP 27633, Activision access 26100; peerchat planned; hosts-file redirect. Does not cover SFC2/OP or WON. https://github.com/Revan67/SFC3Dynaverse

## 3. SFC community solutions

- dynaverse.net: appears down as of 2026-11-02 (no A record). Wayback snapshot 2026-01-18 OK; went dark between Mar and Nov 2026 (could be transient). Dynaverse Gaming Association ran OP/EAW Directory Services. http://www.dynaverse.net/forum/index.php/board,11.0.html
- SFC Launcher by D4v1ks (Carlos), Hot & Spicy Forums: beta Jun 2016 -> v1.0.3.0 Apr 2017; "a Dynaverse and GameSpy's service replacement": emulates Dynaverse directory/firewall on 15101, 26100 (15300), GameSpy 28900, 29900, 29901 (27900), PeerChat 6667, web 80; requires editing `WONServerSetup.gf` and `ServersSetup.gf`. Supports OP 2552/2563, EAW 2036/2037, SFC3 534(b). Download links removed; last post Aug 2017. 2.564 server kit not public. https://hotandspicyforums.com/sfc-launcher-a-dynaverse-and-gamespy-s-service-rep-t19331.html
- OP 2.564 package (H&S team: D4v1ks, TarMinyatur, Adam, Javora, DarkDrone, Falconer): full 576 MB game, patched exe + sprites.q3, HD backgrounds; needs original serial; thread 2018-2019, locked. https://hotandspicyforums.com/starfleet-command-orion-pirates-2500-2564-patch-t25292.html ; Lutris installer "SFCOP v2564 + OPplus411".
- "Community Edition" is SFC2:EAW 2.679, not OP. https://gog.com/forum/star_trek_starfleet_command/to_be_stickied_sfc_community_patches
- Hot & Spicy Forums alive (posts Nov 2026). 2022 thread on local SFC3 Dynaverse: CD key via registry, ValidateClientFiles CRC fix. https://hotandspicyforums.com/no-cd-key-when-logging-to-a-local-dynaverse-server-t39486.html
- GitHub repos on SFC2/OP/Dynaverse RE: none found. "SFC_Editor" not found under that name; D4v1ks' tools (launcher, Sprites.q3 editor) are on H&S.
- Public Dynaverse II server in 2026: no evidence.

## 4. ServerPlatform.exe (Dynaverse II server kit)

- Official OP kit: "Dynaverse II Kit v2552, Server Kit with SQL Support, Build 37" (`SFCOPServer2552_B37.zip`); an older kit without SQL used a flat-file database; "both fundamentally the same". https://archive.kontek.net/sfc.strategyplanet.gamespy.com/dyna_server.shtml
- Local check (this session): `Assets/ServerProfiles/Database.gf` has `UseSQL = 0` (flat file) and an optional ODBC `DSN = "SFC2;"`. `ADOManager.cpp` is in the server's source paths.
- EAW beta kit readme: `ServerPlatform.exe -install` first; run under `ProcessSentinel.exe`; campaign in `MyCampaign.mct`; Map Editor included but undocumented. https://www.fileplanet.com/archive/p-31782/SFCII-DynaverseII-Beta-Server-Kit
- No "Dynaverse 3"/"OpenDynaverse" rewrite for SFC2/OP exists; "Dynaverse 3" is SFC3's official service name.

## Recommended next steps (agent's)
1. Pull `aheadley/homeworld` `src/Titan/{msg,auth,crypt}` headers to decode SFC:OP's WON Dir/Auth/Routing/Firewall messages.
2. Use `FlashZ/won_oss_server` as the Dir/Auth1/Firewall base; `openspy-core` for GameSpy.
3. Database question settled locally: flat file by default.
