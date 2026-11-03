# DirectPlay 4 usage inventory (P1-7)

Static inventory of how `StarFleetOP.exe` 2.5.5.2 uses DirectPlay 4, what sits on top of it, and
what a UDP replacement must provide. Every address is a retail virtual address. Where a statement
is an inference rather than something read directly from the code, it says so.

Reproduce: `analysis/netcode/ghidra/run.sh` (see [`README.md`](README.md)). Names for the shared
Ghidra project: [`directplay_names.csv`](directplay_names.csv). Address tables:
[`results/`](results/).

## Summary

- **One class, one object.** All DirectPlay use is inside one C++ class that has no RTTI (its
  vtables have no complete object locator), so its original name is unknown. This document calls it
  *the wrapper*; the names CSV uses the prefix `dpw_`. Its code spans **0x006104D0 to 0x0061B7F0**,
  wider than the 0x00616700 to 0x00619DC0 the brief started from. There is exactly one instance,
  a static object at **0x00C3B478** built by a static initialiser. It registers itself in a
  singleton pointer at 0x00C3B628.
- **It implements an abstract interface.** The constructor (0x0060F950) installs the vtable at
  0x007B654C first: 41 slots, 39 of them `_purecall`. Then it installs the wrapper's own 41-slot
  vtable at 0x007B64A8. The game only ever calls the wrapper through those 41 virtual slots. That
  interface is the seam a replacement transport plugs into.
- **One client.** `tNetControl` (RTTI; translation unit `Source\network\netcon.cpp`) gets the
  pointer from the singleton accessor 0x0060F880 and keeps it at `tNetControl + 0x8D00`. A
  whole-program scan finds no other code that touches the object, the singleton pointer or that
  member. The lobby UI states and the game-message code reach DirectPlay only through
  `tNetControl`.
- **DirectPlay is the session transport for multiplayer outside Dynaverse.** The connection kinds
  are IP, IPX, Modem, Serial, Null and DPSim. Sessions are created with
  `DPSESSION_DIRECTPLAYPROTOCOL | DPSESSION_KEEPALIVE`, plus `DPSESSION_MIGRATEHOST` for IP and
  IPX.
- **Dynaverse and WON traffic bypass it.** No function in the twelve `Switch*.cpp` translation units
  (nSwitch) references the wrapper. The wrapper itself makes no Winsock call: its whole OS surface
  is COM, threads, events, critical sections and timers. One question stays open: whether a
  Dynaverse *mission* opens its combat session through `tNetControl`. See "Open questions".
- **`tDirectPlayError` never escapes.** There are 20 throw sites and 15 catch handlers, in 15
  wrapper functions. Every handler catches in the same function that threw, logs `what()` and
  continues. Callers get translated result codes, not exceptions.

## Method

1. `run.sh` imports the shared program export (`work/ghidra/StarFleetOP_2552.gzf`) into a private
   project under `work/directplay/`. Both scripts run with `-readOnly`, so nothing is saved.
2. `DpDump.java` lists every function in 0x0060F000 to 0x0061B800 with its callers and callees,
   and every indirect `CALL [reg+disp]` (slot = disp / 4). It dumps the two vtables and decompiles
   everything. The shared program has 25 of the wrapper's 41 slot targets, and 2 of the base's, as
   undefined code; the script creates them in memory first. It also walks the MSVC EH records for
   `direct_play::tDirectPlayError` by searching memory for its type descriptor (0x0087C590).
3. `DpUsers.java` finds every reference to the static object and the singleton pointer, and every
   instruction whose memory operand uses displacement 0x8D00. For each such function it recovers
   the wrapper slots called, by register tracking (`r <- [x+0x8D00]`, `v <- [r]`, `call [v+N]`)
   and from the decompiled C, and lists the function's callers.
4. COM slot numbers were resolved against the public headers of the Wine project:
   [`include/dplay.h`](https://github.com/wine-mirror/wine/blob/master/include/dplay.h) and
   [`include/dplobby.h`](https://github.com/wine-mirror/wine/blob/master/include/dplobby.h). They
   declare `IDirectPlay4` and `IDirectPlayLobby3` in the DirectX 6/7 SDK method order (`IDirectPlay4`:
   53 slots, `IDirectPlayLobby3`: 19). The binary confirms the order independently. Every wrapper
   that carries a method-name error string calls the slot the header gives for that method:
   EnumConnections 35, InitializeConnection 38, EnumSessions 13, Open 24, CreatePlayer 6,
   DestroyPlayer 9, CreateGroup 5, GetPlayerAddress 18, Send 26 and SendEx 49 on `IDirectPlay4A`;
   CreateCompoundAddress 14 and EnumAddress 5 on `IDirectPlayLobby3A`. Constants (`DPSEND_*`,
   `DPSYS_*`, `DPSESSION_*`) come from the same header. DPERR values were read from the game's
   own HRESULT-to-text table at 0x006173A0.

## The wrapper object

| Offset | Content | Evidence |
|---|---|---|
| +0x00 | vtable 0x007B64A8 | constructor 0x0060F950 |
| +0x08 | 16-byte GUID, initialised from 0x007BB028 | constructor; slot 8 returns a pointer to it |
| +0x18 | DPID of the internal group (multicast server mode) | `CreateGroup("InternalSessionGroup")` in slot 5 |
| +0x40 | connection kind: 0 none, 1 IP, 2 IPX, 3 Modem, 4 Serial, 5 Null, 6 DPSim | slot 2 compares the names; assert `fCurrentConnection != kNoConnection` |
| +0x44 | `IDirectPlay4A*` (`fDirectPlayObject`) | `CoCreateInstance` target in 0x006169E0; assert strings |
| +0x48 | `IDirectPlayLobby3A*` (`fDirectPlayLobby`) | `CoCreateInstance` target in 0x00616AC0 |
| +0x5C | map connection kind -> connection description (`fConnectionDescriptions`) | filled by the EnumConnections callback 0x00619DC0 |
| +0x70 | session list (`fSessionList`) | EnumSessions callback 0x00619BC0 |
| +0x84 | player map: entries hold the DPID at +0 and the game id at +0x20 | 0x00618480 lookup, slots 31/32 |
| +0xA8, +0xAC | receive buffer size and pointer | `mReceiveMessages` |
| +0xB4, +0xB8 | inbound message queue and count (lock at +0x168) | 0x006184F0 push, 0x00616470 pop |
| +0xBC | event sink (set by slot 33) | events below |
| +0xD8 | local player's DPID | `SendEx` "from"; `CreatePlayer` result |
| +0xF8, +0xFC | local game id; ready flag | slots 15, 16, 21 |
| +0x138 | byte: `[Network] MulticastServer` from the INI | slot 25 |
| +0x14C, +0x154 | bytes sent; time of first send | `dpw_Send`, `dpw_SendEx` |
| +0x158 | receive rate estimate (float) | 0x00617EB0; slot 40 |
| +0x15C, +0x160, +0x164 | DirectPlay message event (passed to `CreatePlayer`), stop event, receive thread | 0x006182B0 |

## COM creation

| Site | Call | Notes |
|---|---|---|
| 0x006169E0 | `CoCreateInstance(CLSID_DirectPlay, NULL, CLSCTX_INPROC_SERVER\|CLSCTX_LOCAL_SERVER, IID_IDirectPlay4A, this+0x44)` | lazily, before any IDirectPlay4A use |
| 0x00616AC0 | `CoCreateInstance(CLSID_DirectPlayLobby, NULL, CLSCTX_INPROC_SERVER, IID_IDirectPlayLobby3A, this+0x48)` | lazily, before any lobby use |
| 0x00616700 | `CoCreateInstance(CLSID_DirectPlay, ..., IID_IDirectPlay4A, &temp)` | a temporary object to enumerate modem addresses, released at the end |
| 0x00616C80 | `IDirectPlayLobby3A::ConnectEx(flags, IID_IDirectPlay4A, this+0x44, NULL)` | game launched from a lobby application |

`CoInitialize(NULL)` is called in slot 25 (0x00611770) and `CoUninitialize` in the destructor
0x00610000. `ole32` is in the import table; `dplayx.dll` is not, because COM loads it. GUIDs:
`CLSID_DirectPlay` 0x007BB118, `IID_IDirectPlay4A` 0x007BB128, `CLSID_DirectPlayLobby` 0x007BB0B8,
`IID_IDirectPlayLobby3A` 0x007BB0C8. The second data reference to two of them (0x00619E01,
0x00619E09) is inside the EnumConnections callback at 0x00619DC0, which Ghidra has not made a
function.

## IDirectPlay4A methods used

Every call goes through `[this+0x44]`. Wrapper names are the `dpw_` names in the CSV.

| Slot | Method | Wrapper | Arguments and handling |
|---:|---|---|---|
| 2 | Release | 0x00616AA0, 0x00616700 | |
| 3 | AddPlayerToGroup | 0x00615FF0 | local player to the internal group |
| 4 | Close | 0x00615390 | after CancelMessage and two GetMessageQueue calls |
| 5 | CreateGroup | 0x00615EC0 | name "InternalSessionGroup", flags from caller (0x1000 = `DPGROUP_HIDDEN` from slot 5) |
| 6 | CreatePlayer | 0x00615520 | name, hEvent = +0x15C, 0x24-byte player data, flags (0x200 `DPPLAYER_SPECTATOR` from slot 17) |
| 7 | DeletePlayerFromGroup | 0x006160A0 | |
| 8 | DestroyGroup | 0x00616150 | |
| 9 | DestroyPlayer | 0x00615A00 | |
| 11 | EnumGroups | 0x00615E00 | callback 0x0061A180 |
| 12 | EnumPlayers | 0x00615140 (`mDPEnumPlayers`) | callback 0x0061A180; result list at +0x80 |
| 13 | EnumSessions | 0x006149B0 | timeout = `DPCAPS.dwTimeout` (guaranteed), +1000 ms when under 5000; adds `DPENUMSESSIONS_RETURNSTATUS` (0x80) unless byte +4 is set; callback 0x00619BC0 |
| 14 | GetCaps | 0x00616EE0, 0x00616FB0 | flags 0 or `DPGETCAPS_GUARANTEED` |
| 18 | GetPlayerAddress | 0x00616700 | temporary object, modem only |
| 20 | GetPlayerData | 0x00616D30 | `DPGET_REMOTE`; data is 0x24 bytes |
| 21 | GetPlayerName | 0x00615C10 | size query, retry on `DPERR_BUFFERTOOSMALL` |
| 22 | GetSessionDesc | 0x00616540 | size query, retry |
| 24 | Open | 0x00614D70 | `DPOPEN_CREATE` from `mDPCreateSession`, `DPOPEN_JOIN` from `mDPJoinSession` |
| 25 | Receive | 0x0061A610 (`mReceiveMessages`) | `DPRECEIVE_ALL`; grows its buffer on `DPERR_BUFFERTOOSMALL` |
| 26 | Send | 0x006161F0 | wrapper control frames (types 2 and 3); tolerates `DPERR_BUSY` |
| 29 | SetPlayerData | 0x00616E30 | 0x24 bytes, `DPSET_GUARANTEED` |
| 30 | SetPlayerName | 0x00615D00 | |
| 31 | SetSessionDesc | 0x00616620 | |
| 35 | EnumConnections | 0x00614520 | application GUID from 0x004307C5; callback 0x00619DC0 |
| 38 | InitializeConnection | 0x00614630, 0x00616700 | address from `CreateCompoundAddress` |
| 46 | GetPlayerFlags | 0x00615140 | tests `DPPLAYER_SPECTATOR` |
| 49 | SendEx | 0x00616330 | all game data; priority 0, timeout 1000 ms; tolerates `DPERR_BUSY` and `DPERR_PENDING` |
| 50 | GetMessageQueue | 0x00615390, 0x00612420, 0x006124C0 | send queue (1) and receive queue (2) counts |
| 51 | CancelMessage | 0x00615390 | (0, 0): cancels everything before Close |

Not used: Initialize, SecureOpen, StartSession, SendChatMessage, group-in-group, owner and
connection-settings methods, CancelPriority, GetGroupData/Name, GetMessageCount, GetPlayerCaps,
GetPlayerAccount.

### Session descriptions

`mDPCreateSession` (0x00614F10) fills a `DPSESSIONDESC2` (`dwSize` 0x50):

- `dwFlags` = 0x2040, which is `DPSESSION_DIRECTPLAYPROTOCOL | DPSESSION_KEEPALIVE`. It adds
  `DPSESSION_MULTICASTSERVER` (0x800) if `[Network] MulticastServer` is set. Otherwise it adds
  `DPSESSION_MIGRATEHOST` (0x4) when the connection kind is IP or IPX.
- `guidApplication` comes from the caller (0x004307C5).
- `dwMaxPlayers` is the caller's value masked to 16 bits.
- The session name and password come from the caller; `dwUser1..4` are filled by 0x00617CD0.

`mDPJoinSession` (0x00615060) sets only `dwSize`, `guidInstance` and the password, and opens with
`DPOPEN_JOIN`.

## IDirectPlayLobby3A methods used

Every call goes through `[this+0x48]`, except the temporary in 0x00616700.

| Slot | Method | Wrapper |
|---:|---|---|
| 2 | Release | 0x00616B70 |
| 5 | EnumAddress | 0x00616700 (modem addresses; callback 0x0061A2C0) |
| 8 | GetConnectionSettings | 0x00616B90 |
| 12 | SetConnectionSettings | 0x00616C20 |
| 14 | CreateCompoundAddress | 0x00614630 (size query, then build) |
| 15 | ConnectEx | 0x00616C80 |
| 18 | WaitForConnectionSettings | 0x00616C50 |

## The wrapper's interface (vtable 0x007B64A8)

The *callers* column comes from `results/users_vcalls.tsv`. All callers are `tNetControl`
functions; `-` means no caller was found.

| Slot | Target | What it does | Called from |
|---:|---|---|---|
| 0 | 0x0060FCE0 | scalar deleting destructor | - |
| 1 | 0x00610250 | enumerate connections (EnumConnections), keep valid descriptions | 0x004324DB |
| 2 | 0x006104D0 | select connection kind by name (IP, IPX, Modem, Serial, Null, DPSim) | 0x004325B3 |
| 3 | 0x00610810 | release the DirectPlay object, back to no connection | 0x00433C46 |
| 4 | 0x00610820 | enumerate sessions | 0x00432AA8 |
| 5 | 0x00610860 | host: create session and local player (+ internal group), event 0x1450 | 0x004319CA |
| 6 | 0x00610A00 | join: open session, create local player | 0x00431C89 |
| 7 | 0x00610B80 | leave: cancel, close, destroy player | 0x00431821, 0x00431BF8 |
| 8 | 0x00610F20 | pointer to the GUID at +0x08 | 0x004319CA, 0x00431C89, 0x00433EB7 |
| 9 | 0x00610F30 | elapsed time (wrapper clock) | 0x00432316, `tNetControl::vfunction6/7` |
| 10 | 0x00612730 | clock globals | 0x0043240D, 0x0043242E |
| 11 | 0x00610F50 | update session description (dwUser fields) | 0x00433C7E, 0x00433D8A |
| 12 | 0x00611000 | read session description into the game's session record | 0x00432CF4, 0x00433CAA, 0x00433DE8 |
| 13 | 0x00610D20 | append a 16-byte record at +0x140 (purpose not established) | 0x00431C42 |
| 14 | 0x006111E0 | refresh player map (EnumPlayers, GetPlayerData) | 0x0043127F |
| 15 | 0x006115D0 | local game id (+0xF8) | many |
| 16 | 0x006115E0 | ready flag (+0xFC) | 0x004317C3 |
| 17 | 0x006115F0 | create a spectator player | 0x00433F66 |
| 18 | 0x00611610 | destroy player by game id | 0x00433FCA |
| 19 | 0x00611640 | rename player by game id | 0x00433FE6 |
| 20 | 0x00611670 | find player by name | - |
| 21 | 0x00611750 | set ready, publish local player data (SetPlayerData) | 0x00434042 |
| 22 | 0x00612580 | lobby get/set connection settings | 0x00433E51 |
| 23 | 0x00612710 | lobby wait for connection settings | - |
| 24 | 0x00612680 | lobby ConnectEx, create player, start receive thread | 0x00433EB7 |
| 25 | 0x00611770 | init: CoInitialize, INI `[Network]` options | 0x004315EE |
| 26 | 0x00611880 | shutdown: release everything, stop receive thread | 0x004316CF, 0x00431821 |
| 27 | 0x006118A0 | periodic upkeep: latency probe, receive-rate estimate | 0x00431708 |
| 28 | 0x00611960 | latency probe to all players | 0x00433F3E |
| 29 | 0x00611980 | returns 0 (shared stub, see below) | 0x00431917 |
| 30 | 0x00611990 | assign two strings (before host or join) | 0x004319CA, 0x00431C89, 0x004329B9 |
| 31 | 0x00611AF0 | **send game data** | 0x004321FD |
| 32 | 0x006119C0 | **receive game data** | 0x00431EEA |
| 33 | 0x00611C20 | set event sink | 0x00431917 |
| 34 | 0x00611C30 | copy modem address list | 0x004329B9 |
| 35 | 0x00611F90 | connect by address (InitializeConnection with a compound address) | 0x004326B4, 0x004327B1, 0x004328A7, 0x00432918 |
| 36 | 0x00612390 | modem-only reset of 0x00C3B630 | 0x00431821, 0x004329A1, 0x00433C46 |
| 37 | 0x006123A0 | returns 0 (shared stub, see below) | - |
| 38 | 0x00612420 | send queue length (GetMessageQueue) | `tNetControl::vfunction2` |
| 39 | 0x006124C0 | receive queue length | `tNetControl::vfunction3` |
| 40 | 0x00612560 | receive rate | `tNetControl::vfunction4` |

Slots 29 and 37 point at stubs that do nothing but return 0. Other classes' vtables share them:
0x00611980 is referenced from 0x007B651C (this vtable), 0x007A228C and 0x007BB388, and
0x006123A0 from 0x007B653C and 0x007B9124. Ghidra's class recovery named them after one of those
other classes (`tActionBase::vfunction2`, `tGLDC::vfunction23`). Neither that name nor a wrapper
name fits them, so `directplay_names.csv` leaves both out and the import keeps Ghidra's names.

Slot 25 is called through a `lea` of the member, which neither recovery method sees. It was read
by hand from 0x004315EE.

## Message path

### Sending

1. `tNetControl::fn_004321fd` drains the per-player queue of `tNetMsg` records. Each record is
   written as a `uint16` length followed by the serialised message. Records are packed into one
   buffer at `tNetControl + 0x5DD2`, up to `[tNetControl+0x10] x 536` bytes, or `x 1460` when
   0x00432597 returns true. *Inferred:* 536 and 1460 are the minimum and the Ethernet TCP/IP
   segment sizes.
2. It calls wrapper slot 31 with (to, local game id from slot 15, buffer, length, 0,
   reliability class 2).
3. Slot 31 (0x00611AF0) maps the destination:
   - game id 0: the local player gets a loopback copy, and DirectPlay gets `DPID_ALLPLAYERS`, or
     the internal group in multicast-server mode;
   - game id -1: all players;
   - otherwise: a DPID from the player map, or result 0x834 if the id is unknown.

   It picks `SendEx` flags by reliability class:

   | Class | Flags | Meaning |
   |---|---|---|
   | 0 | 0x600 | `ASYNC \| NOSENDCOMPLETEMSG` |
   | 1, 2 | 0x601 | + `GUARANTEED` |
   | 3 | 0x640 | + `ENCRYPTED` |
   | 4 | 0x620 | + `SIGNED` |

   In connection kind Null (5) everything is looped back locally.
4. `dpw_SendEx` (0x00616330) prepends the wrapper frame header and calls `SendEx(localDPID, to,
   flags, frame, len + 12, priority 0, timeout 1000, NULL, NULL)`. It counts bytes sent.

### Receiving

1. When a session opens, `CreatePlayer` is given the event at +0x15C. A receive thread (0x0061A550,
   started by 0x006182B0) waits on that event and the stop event.
2. On each signal `mReceiveMessages` (0x0061A610) loops `Receive(DPRECEIVE_ALL)` until
   `DPERR_NOMESSAGES`. Messages from `DPID_SYSMSG` go to the system handler (0x0061A6F0); all
   others go to the frame handler (0x0061B050).
3. Data frames (type 1) are queued with {from, to, length, payload} on the inbound queue.
4. `tNetControl::fn_00431eea` polls wrapper slot 32 into a 12000-byte buffer until the queue is
   empty. Slot 32 maps DPIDs back to game ids; it returns 0x401 if the buffer is too small and 0x835
   for an unknown player. The poller splits the bundle back into `uint16`-length records and hands
   each to the `tNetMsg` decoder (0x004308B2).

### Framing

Wrapper frame, as sent through `Send` or `SendEx` (`len + 12` bytes):

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | frame type |
| 4 | 4 | payload length (written by `dpw_SendEx`; `dpw_Send` leaves it unset) |
| 8 | n | payload |
| 8+n | 4 | not written (the frame is allocated 4 bytes longer than it is filled) |

| Type | Sent by | Payload | Receiver |
|---:|---|---|---|
| 1 | `dpw_SendEx`, guaranteed or not per class | `tNetControl` bundle | queued for slot 32 |
| 2 | `dpw_SendProbe` (0x006143C0), `Send`, not guaranteed | {sender time, 0, count} | echoed back as type 3 with the receiver's id and count - 1 |
| 3 | frame handler, `Send` | {time, assigned id, count} | round-trip estimate per player (map at +0x11C), event 0x138C; sends another probe while count > 0 |
| 4 | not found in the wrapper; *inferred:* sent by the host | {target DPID, new game id} | "Change assign ID to %d from %d": updates player data, event 0x138D |

A new player gets a probe with count 10 as soon as `DPSYS_CREATEPLAYERORGROUP` arrives.

Game-level framing: a bundle is a sequence of `uint16 length` + `tNetMsg` body. In
`ghidra/exports/classes.csv`, 81 classes derive from `tNetMsg`: 78 `tNetMsg_k*`, plus
`kNetMsg_CreatePhysicalEntity`, `tNetMsg_Header` and `tShipSystemsInfo`. Counting `tNetMsg` itself
gives 82.
The decoder compares a one-byte kind in the decoded header (values 0x04, 0x2F and 0x6C are tested
in 0x00431EEA). Mapping every kind value to its class is outside this task.

### DirectPlay system messages handled (0x0061A6F0)

| `DPSYS_*` | Action | Event posted to the sink |
|---|---|---|
| `CREATEPLAYERORGROUP` (0x3), players only | adds to the player map, fetches player data, sends a probe | 5000 (0x1388) |
| `DESTROYPLAYERORGROUP` (0x5), players only | removes from the player map | 5001 (0x1389); 0x1451 also when Modem and not a spectator |
| `SESSIONLOST` (0x31) | GetSessionDesc | 0x13ED |
| `HOST` (0x101) | GetSessionDesc | 0x13EF (this machine became host) |
| `SETPLAYERORGROUPDATA` (0x102), players only | copies the new data | - |
| `SETSESSIONDESC` (0x104), same `guidInstance` | | 0x13EE |
| 0x0A | calls the wrapper's own vtable slot 22 (0x58) with type 6 | - |

0x0A matches no `DPSYS_*` value in the header. *Inferred:* it is unreachable or a decompiler
artefact.

The sink is an object with three methods (vtable +4: post(id, record), +8: post(id, a, b, c),
+0xC: notify(id)); `tNetControl` installs it through slot 33. Other event ids:

- 0x1450: session hosted (slot 5).
- 0x1451: also the translated result for two DPERR values in 0x00617960.

## Errors

`direct_play::tDirectPlayError` (0x20 bytes) derives from
`ExpandedExceptions::TWindowsAPICallFailed`, which derives from `TRuntimeError`, then `TException`,
then `std::exception`. The chain comes from the CatchableTypeArray of ThrowInfo 0x00823470. The
constructor (0x00614460) takes the HRESULT, keeps it at +0x1C and, when the location flag is set,
formats it with the game's DPERR text table (0x006173A0).

Throws and catches (`results/eh.tsv`): 20 throws of ThrowInfo 0x00823470 and 15 catch handlers
(`HandlerType` adjectives 8, by reference), in:

| Function | Throws | Catches |
|---|---:|---:|
| 0x00616700 | 5 | 1 |
| 0x00614630 | 4 | 1 |
| 0x006111E0, 0x00614520, 0x006149B0, 0x00614D70, 0x00614F10, 0x00615060, 0x00615140, 0x00615520, 0x00615A00, 0x00615EC0, 0x006161F0 | 1 each | 1 each |
| 0x00615C10, 0x00615D00 | 0 | 1 each |

Each handler calls the exception's virtual slot 1 (`what()`), passes the text to the logger
0x006123B0 and resumes after the try block. No `tDirectPlayError` leaves the wrapper. Programming
errors (null object, missing session list) throw `TLogicError` (ThrowInfo 0x008232C0, 34 sites).
COM and thread failures throw `TWindowsAPICallFailed` (ThrowInfo 0x00823548, 4 sites). Neither is
caught in the wrapper. Results that reach `tNetControl` go through 0x00617960, which maps DPERR
codes to game codes, for example `DPERR_BUSY` to 0x834.

## Call graph summary

```
multiplayer setup and lobby UI (tState_*), game code
        |
tNetControl  (netcon.cpp; 0x0043104E to 0x004343B4; member +0x8D00)
        |  41-slot virtual interface (abstract base vtable 0x007B654C)
wrapper  (0x006104D0 to 0x0061B7F0; static object 0x00C3B478)
        |  COM
IDirectPlay4A / IDirectPlayLobby3A  (dplayx.dll, loaded by CoCreateInstance)
```

- 43 `tNetControl` functions call wrapper slots (42 found by the scripts, plus 0x004315EE).
  Their callers are in `results/users_vcalls.tsv`, last column. 74 distinct functions outside
  `tNetControl` call them, and 47 of those lie in 0x00450000 to 0x0047FFFF. The evidence that this
  range is the multiplayer setup and lobby UI comes from `ghidra/exports/vtables.csv` and
  `strings_to_functions.csv`:
  - Three callers are vtable methods of UI states:
    - 0x00461EEA `tState_CustomizeMultiplayer`
    - 0x00464D4F `tState_GameBrowser`
    - 0x0047BA63 `tState_SelectGame`
  - Others reference the lobby's resource and INI strings:

    | Caller | Strings it references |
    |---|---|
    | 0x00465920, 0x0047422C | "SessionError", "JoinStatus" |
    | 0x00465A71, 0x00474891 | "GameConnections" |
    | 0x004669C6 | "Modem", "Serial", "GameBrowser" |
    | 0x004682C7 | "Network", "DPSim", "MPlayer" |
    | 0x004752B3 | "Modem", "Serial", "IPandIPX" |
    | 0x00473E91 | "CustomizeMultiplayer", "multiplayer" |

  In all, 36 of the 47 have a state vtable or such strings; the other 11 are *inferred* to belong
  with them by address. The remaining callers lie outside that range. They include `tNetMsg_k*`
  code (for example 0x0046ED74, 0x004ABAE7, 0x0052B331) and 0x0040AB7E, 0x004B811F and
  0x00534FCB.
- Several `tNetControl` functions that use the wrapper are mislabelled by class recovery as
  `tNetMsg_Header::~tNetMsg_Header` or `tNetMsg::~tNetMsg`: 0x00431EEA (receive loop) and
  0x0043405F / 0x00434195 (`tNetControl::mQueueMsg`, named by its assert string). The CSV renames
  them.
- Several entry points (host 0x004319CA, join 0x00431C89, connect-by-address 0x004326B4 to
  0x00432918, enumerate 0x00432AA8) have no recorded callers. They use `unaff_EBP` frames, so they
  are probably fragments of larger functions Ghidra split. *Inferred*, not checked: their parents
  are the lobby states that reference "DIRECTIP", "Modem Session", "Serial Session" and the INI
  keys `SessionName` and `SessionType`.

## Dynaverse and WON

- **Supported by the evidence:** nSwitch (the twelve `Switch*.cpp` units, `WONInterSwitchConnection.cpp`
  among them) never references the wrapper object, its singleton pointer or `tNetControl+0x8D00`.
  The wrapper never calls Winsock. Dynaverse server traffic and WON traffic therefore do not go
  through DirectPlay. This agrees with the proposal's statement (section 2.3, Winsock table) that
  WON, GameSpy and Dynaverse use nSwitch over BSD sockets.
- **Contradicted:** three places in `PROPOSAL.md` (line numbers as on `develop` at a8ad968) need an
  edit. In fact skirmish, LAN and Direct-IP play go through `tNetControl` and the DirectPlay
  wrapper.
  - Line 19 says "Dynaverse and lobby traffic use raw Winsock" through nSwitch. That is true of
    the Dynaverse lobby, but not of the LAN and Direct-IP lobby (`tState_GameBrowser`,
    `tState_CustomizeMultiplayer`), which runs on DirectPlay. It also gives the region as 14 KB;
    the wrapper spans 0x006104D0 to 0x0061B7F0, about 45 KB.
  - Line 116 says skirmish, LAN and Direct-IP play and Dynaverse missions all use `nSwitch` over
    Winsock. Skirmish, LAN and Direct-IP do not. Dynaverse missions are open (below).
  - Line 409 ("Changes from the earlier plan") says "DirectPlay is not involved". It is: it is the
    session transport outside Dynaverse.

  Line 70 (DirectPlay is the LAN and Direct-IP session transport) is correct. Its range
  (0x616700 to 0x619DC0) is narrower than the wrapper's.
- **Open:** whether a Dynaverse mission, once the server has matched players, opens its combat
  session through `tNetControl` (DirectPlay over IP) or through nSwitch. Leads:
  - the "MetaGameSession" string (0x005ED93B);
  - `tNetControl` callers in the 0x0053xxxx range (0x00534FCB calls slots 11, 29 and 33 via
    0x00433D8A and 0x00431917).

  A runtime trace from the replay oracle would settle it.

## What a UDP replacement must provide

The replacement implements the 41-slot interface, as a new implementation of the abstract base at
0x007B654C, and leaves `tNetControl` and everything above it unchanged. From the inventory above,
it must provide:

1. **Connections.** A list of connection kinds and selection by name (slots 1, 2, 35). Only IP
   needs a real implementation. IPX, Modem, Serial and DPSim can report "unavailable". Null (local
   loopback, kind 5) must keep working, because single-machine play uses it.
2. **Session discovery.** LAN broadcast plus direct address ("DIRECTIP"), returning per session:
   instance GUID, application GUID, name, password-required flag, current and max players, and four
   user dwords (slots 4, 12). The enumeration timeout semantics (about 1 to 5 s) should match.
3. **Session create and join.** Host with name, password, max players (16-bit) and user dwords;
   join by instance GUID and password; leave (slots 5, 6, 7, 11).
4. **Players.** Create and destroy, names, spectator flag, 36 bytes of per-player data replicated
   reliably to everyone (`SetPlayerData` with `DPSET_GUARANTEED`), and a stable id per player.
   Join and leave notifications (events 5000 and 5001). Slots 14, 17 to 21.
5. **Sending.** To one player, to all, or to all but self. Unreliable and reliable (classes 0 and
   1/2); classes 3 and 4 asked DirectPlay for encryption and signing, which can map to reliable.
   Messages up to 1460 x `[tNetControl+0x10]` bytes per bundle; the receiver accepts 12000. Order:
   DirectPlay preserved order per sender for guaranteed messages; the replacement should too.
   A 1000 ms send timeout and the send queue length (slot 38) are used for flow control.
6. **Receiving.** A queue polled from the game thread (slot 32), filled from a network thread, with
   the receive queue length (slot 39) and a receive-rate estimate (slot 40).
7. **Latency.** The probe and reply exchange (frame types 2 and 3) and the per-player round-trip map
   it feeds (event 0x138C). It can be reimplemented inside the transport.
8. **Host migration.** IP and IPX sessions are created with `DPSESSION_MIGRATEHOST`. The game reacts
   to `DPSYS_HOST` (event 0x13EF) and also has its own `tNetMsg_kChangeHost` message. A replacement
   must elect a new host when the host leaves and raise the same event. Session-lost (0x13ED) and
   session-description-changed (0x13EE) events are also consumed.
9. **Keepalive and disconnect detection.** `DPSESSION_KEEPALIVE` was set, so dropped players must be
   detected and reported as leaves.
10. **Id reassignment** (frame type 4), if the host sends it. The sender was not found in the
    wrapper (see open questions).
11. **Lobby launch** (slots 22 to 24) can be dropped. It exists for DirectPlay lobby applications.
    Multicast-server mode (`[Network] MulticastServer`, internal group) is optional.

Not needed: COM, `dplayx.dll`, the DirectPlay service providers, the modem and serial address
code (0x00616700, 0x00618990 to 0x00619750).

## Open questions

- The original class name of the wrapper and of its abstract base (no RTTI; no assert path in the
  region). Their namespace is presumably `direct_play`, from `tDirectPlayError`; that is inferred.
- Which code sends frame type 4 (id reassign). It is not sent by any `Send` or `SendEx` call in the
  wrapper; it may come from `tNetControl` through slot 31 with a raw payload.
- The Dynaverse-mission transport (above).
- The purpose of slot 13 and of the string pair in slot 30.
- 0x00619DC0 (the EnumConnections callback) and 0x0061A180 / 0x0061A2C0 (enumeration callbacks)
  are not functions in the shared program. The CSV does not name them.
