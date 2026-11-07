# Which transport a Dynaverse mission uses (N1, DESIGN §12.6)

**Answer.** A Dynaverse mission's combat session runs on the DirectPlay wrapper through `tNetControl`,
over TCP/IP, connected by address to the host the campaign server chose. nSwitch carries only the
campaign side of a mission: the server's match (`kHostNewMissionMsg`, `kPlayNewMissionMsg`), the
ready-to-play exchange and the mission result. When the mission starts, the client tells nSwitch to
start caching Dynaverse messages, and the state after the mission stops it. *Inferred:* no combat
message goes through nSwitch. Within six calls, RunCampaign reaches nSwitch's code only through that
caching call, plus two helpers that lie in nSwitch's address range but are not nSwitch's ("What
reaches the nSwitch range" below).

Every address is a retail virtual address of `StarFleetOP.exe` 2.5.5.2. A statement that rests on
behaviour rather than on a string, RTTI or the call graph says *inferred*.

Reproduce: `analysis/netcode/ghidra/run.sh` (step 3 runs [`ghidra/DvMission.java`](ghidra/DvMission.java)).
Tables: [`results/dynaverse_rpaths.tsv`](results/dynaverse_rpaths.tsv),
[`results/dynaverse_paths.tsv`](results/dynaverse_paths.tsv),
[`results/dynaverse_edges.tsv`](results/dynaverse_edges.tsv).

## Method

`DvMission.java` walks the call graph of the shared program, read-only, over three kinds of edge:

- direct calls;
- references from code to a function's entry (a pushed or stored callback);
- the fall-through into a function body that the shared program split off after a call to the
  C++ frame-setup helper `0x00736254`, which it takes for non-returning.

The third kind matters. Most C++ functions with unwind state in this exe are two functions in the
shared program, a 10-byte stub (`MOV EAX, <unwind table>; CALL 0x00736254`) and the body after it.
The body has no callers of its own. Without the edge every walk stops at the stub. That is why
`directplay.md` found no callers for `tNetControl`'s host, join and connect-by-address entries.

`run.sh` names regions and walks from them. Forward walks start from the mission controller
(`srcMissionCtrl`, 0x005EE000 to 0x005EF600) and the `tState_RunCampaign` state (`srcRunCampaign`,
0x00479AED to 0x0047B2DC). Backward walks run from four `tNetControl` entries (each stub with its
body) up to the first function in a source region:

- host, slot 5: 0x004319C0 and 0x004319CA;
- join, slot 6: 0x00431C7F and 0x00431C89;
- connect by address, slot 35: 0x0043290E and 0x00432918 (`"DIRECTIP"`);
- enumerate sessions, slot 4: 0x00432CCE.

The other source regions are the multiplayer lobby UI and the rest of the Dynaverse client. The
walks go at most six calls deep.

## The path

1. **The server matches the mission.** The client's mission procedures are
   `nAsyncClientMissionProcedures::tHostNewMissionMsg` (handler 0x005EBFC8, log string
   `"MetaClient: kHostNewMissionMsg"`) and `tPlayNewMissionMsg` (handler 0x005EC259). Each one
   reads a `tMatchedMission` from the nSwitch message and calls the mission controller:
   0x005EE15C/0x005EE166 for the host and 0x005EE1A9/0x005EE1B3 for a client. Both call
   0x005EE800/0x005EE80A with a flag, 1 for the host and 0 for a client. That function stores the
   flag and resets the game setup (0x00534F60). The setup reset clears only a flag on `tNetControl`
   (0x004321EF, `+0x11`). In `results/dynaverse_edges.tsv`, the mission controller's only direct
   call into another region is nSwitch's `~tRequestWithResponse` (0x00716400, from 0x005EEB6E). In
   `results/dynaverse_paths.tsv`, its paths reach `tNetControl` only at two setters: 0x004321EF,
   through the setup reset, and 0x004321F9, through 0x005353F1.
2. **The controller enters the RunCampaign state.** `tMissionCtrlC::StartMultiplayerGame`
   (0x005EE761, log string `"MNE: tMissionCtrlC::StartMultiplayerGame() -> To Briefing Screen"`)
   switches the game state by name, `"RunCampaign"`, through 0x0046A62D. `tState_RunCampaign`
   (vtable 0x0079F634) returns `"RunCampaign"` from slot 12 (0x00479C60). This step goes through the
   state table by name, so the call graph does not see it; the strings and the vtable do.
3. **RunCampaign hosts or joins over DirectPlay.** The state's update, slot 1
   (`tState_RunCampaign::vfunction2`, 0x0047A00B), reaches 0x0047A506 through 0x0047A015 and
   0x0047A4C3. 0x0047A506 calls one of two functions on the global `tNetControl` at 0x008F5B98:
   - **Join, 0x0047A512/0x0047A51C**, gated by `this+0x11FC` and a timer at `this+0x1218`. It asks
     for the connection state (0x004317BD) and enumerates connections (0x0043257A). It selects
     connection 2 (0x004325A9), the same index that the lobby's TCP/IP screen selects (0x00468527,
     strings `"OtherConnectionsTCPIP"` and `"IPAddress"`). It then connects by address
     (0x0043290E → 0x00432918, wrapper slot 35, `"DIRECTIP"`). The address is a string at `+0xFC`
     of the `tMatchedMission` that 0x005EDC3B/0x00427145 return. It enumerates sessions (0x00432CCE,
     slot 4) and sets the player's names from `+0x110` and `+0x120` of the same record (0x00432F3B).
     Last, it joins (0x00431C7F → 0x00431C89, slot 6).
   - **Host, 0x0047A6DE/0x0047A6E8**, gated by `this+0x11FD` and a timer at `this+0x1220`. It shuts
     the previous connection down (0x00431821) and initialises (0x00431917, slots 29 and 33). It
     selects connection 2, connects by address with an empty address (the constant at 0x008930A4),
     and sets the names. It hosts (0x004319C0 → 0x004319CA, slot 5), then publishes the player's
     data (0x00434042, slot 21) and the session description (0x00433C7E, slot 11).

   `results/dynaverse_rpaths.tsv` shows this: from the RunCampaign region, each of the four
   entries is one or two calls away, through 0x0047A51C (join, connect, enumerate) or 0x0047A6E8
   (host, connect). The backward walk reaches the lobby UI by the same entries (0x0045A51A host,
   0x0047685B join, 0x004669C6 connect, 0x00469636 enumerate). It has no row from the mission
   controller or the rest of the Dynaverse client: neither reaches host, join, connect or enumerate
   within six calls. Their forward paths reach `tNetControl` only at the two setters of step 1
   (`results/dynaverse_paths.tsv`). The walk's full output, outside the committed tables, also
   shows one Dynaverse client function, 0x006087B9, calling `tNetControl`'s shutdown (0x00431821).
4. **The mission's messages are `tNetMsg`s on `tNetControl`.** 0x0047AD5B handles the player-join
   message (strings `"I hear a kPlayerJoin of a Client"`, `"I am the host"`, `"I am playerID: "`,
   `"Start Meta message caching"`). It queues messages with `tNetControl::mQueueMsg` (0x0043405F).
   0x0047B260 sends the game speed the same way. The same queue feeds the wrapper's send slot 31
   (`directplay.md`, "Sending").

## What reaches the nSwitch range

`results/dynaverse_paths.tsv` has 28 paths from the RunCampaign region into the nSwitch region
(0x00713000 to 0x0072B000). They end at three functions:

- **21 at 0x0071B990, `std::vector<unsigned short>::insert`** (with its mangled name in
  `ghidra/exports/functions.csv`). They go through `tString`'s constructors and `operator=` (0x00623120,
  0x006231D0, 0x00624660) and `tString::mIncorporateCharList` (0x00623A10), so they are the string
  class's own storage. The function lies in that range only by link order. It is not nSwitch's.
- **2 at 0x00716950**, a 4-byte accessor that the program names
  `nSwitch::tRequestWithResponse::vfunction9`. It is reached from 0x004CAF4A, which passes it the
  same object it then reads with `tString::operator[]` and `tString::substr`. *Inferred:* the
  linker merged identical function bodies and kept one name, so this is the string's length
  getter, not an nSwitch request.
- **5 at 0x00713B20**, which returns the nSwitch singleton (the assertion `"fSwitch.get()"`, from
  `Switch.cpp`). These are the direct calls from 0x0047A96B (0x0047AA4F) and the player-join
  handler 0x0047AD5B (0x0047AECB). Each comes right after logging `"Start Meta message caching"`,
  and each then calls 0x00714170 on the singleton. That function jumps to slot 0x4C of the object
  at the singleton's `+0xB4`. The matching `"Stop Meta message caching"` is in
  `tState_ContCampaign::vfunction4` (0x0047B6E0), the state after the mission. *Inferred:* this
  tells nSwitch to hold incoming Dynaverse messages until the mission ends. It sends no combat
  data.

The campaign server's side agrees. `TaskPrepareMatchedMission.cpp` (0x00581C6F to 0x00582842)
logs `"Client match host at IP: ("` (0x00581EBC), and the server's mission procedures log
`"Informing host "` (0x005794F6). *Inferred:* the server puts the host player's IP address into the
matched mission it sends to the other players, and that address is the string at `+0xFC`.

## What it means for N5 and V1 to V3

The Dynaverse mission needs no transport of its own. Its combat session is the same DirectPlay
session as a Direct-IP game, so N5's work carries it once N5 moves the dplay module onto the
Rendezvous. That work is version 1 of the session messages in `src/net/PROTOCOL.md`. The one new
fact is where the address comes from. A Dynaverse client joins the address string that the
campaign server wrote into the `tMatchedMission`, by `"DIRECTIP"` connect-by-address. On the
Internet, through NAT, a raw IP address of the host is useless. So a Dynaverse server run by this
project (V1 to V3) must put the host's rendezvous name (`src/net/RENDEZVOUS.md`, "Names") in that
field. N5 must then accept such a name where the game passes a connect-by-address string. Both are
to be written into those packages' briefs. With the original server kit under Wine, the field holds
the address the kit saw, which works only on one network or with forwarded ports. The nSwitch
traffic (logon, map, matching, results) is a separate TCP protocol and stays V1 to V3's.

## Limits

- Static analysis only. A runtime trace of a Dynaverse mission (the server kit plus two clients)
  would confirm the order of calls; none exists yet.
- "No combat message goes through nSwitch" is inferred. It rests on the walk: RunCampaign reaches
  nSwitch only through the caching call, within six calls. Deeper paths, and virtual calls through
  objects built outside the walked regions, are not covered. What slot 0x4C of the `+0xB4` object
  does was not decoded; only the log strings around it name it.
- The gates `this+0x11FC` and `this+0x11FD` choose join or host. *Inferred:* they come from the flag
  the mission controller stores (1 for the host). The write that sets them was not traced.
- Index 2 for TCP/IP rests on the lobby's TCP/IP screen using the same index. The table it indexes
  (0x0043263D) was not decoded.
