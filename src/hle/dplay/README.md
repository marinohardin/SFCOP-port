# DirectPlay 4 for LAN

The `IDirectPlay4A` and `IDirectPlayLobby3A` rows of `src/hle/surface.tsv` (53 + 19), on the
Transport, Clock and Threads components (`src/platform/`). The game makes both objects with
`CoCreateInstance` and uses the 28 + 7 methods `analysis/netcode/directplay.md` lists. Native
clients on one network find each other's sessions, join them and exchange the game's messages
unchanged. This is the LAN path only: Internet play, NAT traversal, the lobby service and the relay
come later (DESIGN §5.11, §6.7). The protocol between clients is this project's own and does not
interoperate with Microsoft's DirectPlay.

| File | What it is |
| --- | --- |
| `DplayDefs.h` | Result codes, flags, system message types, GUIDs and the structures as values |
| `Bytes.h` | A little-endian writer and a bounds-checked reader |
| `Address.h`, `Address.cpp` | DirectPlay addresses: chunks led by `DPAID_TotalSize`, the TCP/IP connection and a player's address |
| `Layout.h`, `Layout.cpp` | `DPNAME`, `DPSESSIONDESC2` and the `DPMSG_*` system messages in the 32-bit layout the game reads |
| `Wire.h`, `Wire.cpp` | The LAN protocol's datagrams (below) |
| `DirectPlay.h`, `DirectPlay.cpp`, `DirectPlayNet.cpp`, `DirectPlayLock.h` | `IDirectPlay4A`: sessions, players, groups, messages; the packet handling and timers |
| `Lobby.h`, `Lobby.cpp` | `IDirectPlayLobby3A`: the address methods; the lobby-launch methods answer `DPERR_NOTLOBBIED` |
| `DplayRuntime.h`, `DplayRuntime.cpp` | `setPlatform()` for the runtime, and the `Environment` an object is built on |
| `DplayGuest.cpp` | The shims, `sfcop_hle_dplay[]`: every method of both interfaces; built only with the lift |
| `DplayTestUtil.h`, `DplayMockFixture.h` | Test helpers: an in-memory network of several machines, and a hand-written host on `TransportMock` |
| `SessionTest.cpp`, `PlayerTest.cpp`, `MessageTest.cpp`, `LobbyTest.cpp` | Every entry through `TransportMock`, with the datagrams written out by hand |
| `WireTest.cpp`, `AddressTest.cpp`, `LayoutTest.cpp` | The byte formats, by hand |
| `LanTest.cpp` | Several objects on the in-memory network: joins, the mesh, loss, host migration, timeouts |
| `DplayRuntimeTest.cpp` | `setPlatform()`, the players' events, the service thread |
| `DplayShimTest.cpp` | Every row has a shim that purges the bytes the row gives, in slot order |
| `tools/LanPairTest.cpp` | Two processes on one machine on the real Transport: enumerate, join, exchange the first three message kinds |

## How a session works

One object is one machine in at most one session. The machine that opens a session with
`DPOPEN_CREATE` is its host: it listens on the enumeration port (47700) and admits joiners.
`EnumSessions` sends a request to the broadcast address and to 127.0.0.1 (or to the host an
`INet` address names) every 250 ms for its timeout, and reports each host that answers.
`Open(DPOPEN_JOIN)` asks the host of an enumerated session. The host refuses a full session, a
closed one or a wrong password with the error the game gets. Otherwise it gives the joiner a
peer number, the session and its name table, and it tells the other machines about the joiner.

Every machine then talks to every other one directly. A machine owns the players and groups it
creates and tells the others about them. An id is the owner's peer number in the high 16 bits and
the owner's own counter in the low 16, so creating one needs no round trip. These travel on a
per-peer channel that is sequenced, acknowledged and retransmitted after 200 ms:

- guaranteed messages (`DPSEND_GUARANTEED`, and the signed and encrypted forms);
- every change to the name table;
- player data, guaranteed or not, so that every machine holds the same data.

Other messages are single datagrams. A machine that sends nothing for a second sends a keepalive.
A machine not heard from for ten seconds is gone: its players are destroyed everywhere. If the
host goes, a `DPSESSION_MIGRATEHOST` session gets as its new host the remaining machine with the
lowest peer number. That machine opens the enumeration port and its players get `DPSYS_HOST`.
Without that flag, the session is lost (`DPSYS_SESSIONLOST`).

`Receive` hands out the oldest message the flags select. System messages are laid out as the game
reads them, with their strings and data after the structure. A local player whose
`CreatePlayer` gave an event gets it signalled for each message. With `Threads`, an open session
has a service thread that keeps it alive while the game makes no call. Without it, every call
services the object.

## Wire format

UDP, little-endian, one message per datagram. Every datagram starts with the magic `53 46 44 50`,
the version 1, its type and two zero bytes. Datagrams between members of a session add the session
key (four bytes, from the instance GUID) and the sender's peer number. A string is a 16-bit length
followed by its bytes; the length `FFFF` means no string. Bytes are a 32-bit length followed by
the bytes.

| Type | Datagram | Body |
| --- | --- | --- |
| 1 | Enumeration request | request id, application GUID (null: any) |
| 2 | Enumeration reply | request id, session description without its password (`DPSESSION_PASSWORDREQUIRED` set if it has one) |
| 3 | Join request | instance GUID, password |
| 4 | Join refusal | instance GUID, the `HRESULT` |
| 5 | Channel message | sequence number, then one inner message |
| 6 | Acknowledgement | the highest sequence number taken in order |
| 7 | Game message | from, to, the game's bytes |
| 8 | Keepalive | |
| 9 | Leave | |

A session description is flags, instance, application, the player limit, the current count, the
four user words, the name and the password. An inner message is a type byte and then:

| Type | Inner message | Body |
| --- | --- | --- |
| 1 | Join accept | your peer number, the next peer number, the session, the other machines (number, address, port), the name table |
| 2 | Machine added | number, address (0: the address you reach the host at), port |
| 3 | Machine removed | number |
| 4 | Player or group added | id, group flag, flags, owner, short and long name, data, members |
| 5 | Player or group removed | id |
| 6 | Data set | id, data |
| 7 | Name set | id, short and long name |
| 8, 9 | Member added, removed | group, player |
| 10 | Session description set | the session |
| 11 | Guaranteed game message | from, to, the game's bytes |

Every decoder checks bounds and refuses a datagram that is malformed or has trailing bytes.

## How the game reaches it

The game calls `CoCreateInstance(CLSID_DirectPlay)` and `CoCreateInstance(CLSID_DirectPlayLobby)`.
`DplayGuest.cpp` registers both classes with win32hle's `hle_com_register_class` when the program
loads, so `CoCreateInstance` calls this module's create functions. The runtime gives the
components with `sfcop::hle::dplay::setPlatform()`. Until it does, `CLSID_DirectPlay` answers
`REGDB_E_CLASSNOTREG`, as on a machine without DirectPlay, and the lobby object still works.

A blocking method (`EnumSessions`, `Open`) gives up the machine while it waits. Callbacks into the
game run on the calling thread, with what they are handed in that thread's scratch memory. A
player's event is signalled through `Threads` when the runtime resolves its handle, and otherwise
through win32hle's `SetEvent`.

## Tests

```sh
build/sfcop_tests -sf="*hle/dplay*"
```

`tools/LanPairTest.cpp` is the one two-process test: it forks a second process, and the two
talk over real loopback sockets on the host Transport backend (`TransportHost.cpp`) and wait on
the steady clock, so it is the one test that uses a real clock (`docs/CONVENTIONS.md` §6). It
runs in `sfcop_tests` on every platform that has the backend and `fork()`. Where it has neither,
or the backend opens no UDP socket, the test case prints a line starting `skipped:` and returns;
a failed check kills the child at once.

## Known differences

- No fragmentation: a message is at most 60000 bytes (`dwMaxBufferSize`), and a join accept must
  fit in one datagram (`DPERR_CANTADDPLAYER` otherwise).
- Priorities, timeouts and `CancelMessage` do nothing: no message waits in a send queue.
  `GetMessageQueue(DPMESSAGEQUEUE_SEND)` counts guaranteed messages not yet acknowledged.
- `DPENUMPLAYERS_SESSION` (the players of a session not joined) answers `DPERR_UNSUPPORTED`.
- A machine changes only what it owns. A peer's change to a player or group (name, data, removal)
  is applied only if the sender owns it; a membership change only if the sender owns the group
  or the player; an announced entity must be the sender's own, with an id from its range. The
  same two rules bind this machine's own `AddPlayerToGroup` and `DeletePlayerFromGroup`
  (`DPERR_ACCESSDENIED`), which real DirectPlay does not require. Anything else is dropped and
  counted (`DirectPlay::GetCounters()`).
- Limits, each with a counter: 4096 messages waiting for `Receive` (the oldest is dropped), 256
  sessions remembered by an enumeration (the oldest), 4096 unacknowledged channel messages per
  peer (the oldest; that peer is dropped by its timeout soon after), 1024 players and groups
  announced by one peer (further ones refused).
- When the host is lost in a session without `DPSESSION_MIGRATEHOST`, the object is in a lost
  state after `DPSYS_SESSIONLOST`: calls that need a session answer `DPERR_SESSIONLOST` until
  `Close()`; the code is `DPERR_SESSIONLOST`, the one this module defines.
- The join password travels in plaintext in the join request on the LAN; the wire protocol is
  this project's own and does not protect it.
- A session with a password is always listed, flagged `DPSESSION_PASSWORDREQUIRED`.
- System messages go to local players only, and only when there are some. A name, data or
  session description change is reported only to the other machines.
- The methods the game never calls answer `E_NOTIMPL`. The lobby-launch methods answer
  `DPERR_NOTLOBBIED`, and `RunApplication` answers `DPERR_UNSUPPORTED`.
- The shims are checked as text, as `src/hle/miles/README.md` describes.
