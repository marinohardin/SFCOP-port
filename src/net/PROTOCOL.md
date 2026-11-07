# Network protocol between native clients

The on-wire format of a game session between native clients, as DESIGN §6.7 states it. There are
two layers:

- **Session messages.** These are the datagrams of the DirectPlay HLE module (`src/hle/dplay/Wire.h`).
  They carry the game's `tNetMsg` bundles unchanged, plus the session, player and group bookkeeping
  that DirectPlay did. They are version 1 below, exactly as the LAN module sends them today.
- **The envelope.** Over the Internet, every session message travels inside one envelope, whether
  it goes direct or through the relay. The envelope adds the lobby's session id, the two peer ids,
  a sequence number, a reliability flag and fragmentation.

The protocol is this project's own. It does not interoperate with Microsoft's DirectPlay or with
the stock game (DESIGN §5.11). The lobby's HTTPS API is in `LOBBY_API.md`. The UDP rendezvous, hole
punching and the relay's own messages are in `RENDEZVOUS.md`. The choice of NAT traversal
implementation is in `NAT_LIBRARY.md`.

## Reading the tables

- Every integer is little-endian, as in `Wire.h`. An IPv4 address is the `u32` of its host-order
  value, as `platform::Endpoint` holds it: 192.0.2.1 is `0xC0000201`, on the wire `01 02 00 C0`.
- Types: `u8`, `u16`, `u32`, `u64`. `u8[N]` is N bytes. `guid` is 16 bytes in the GUID's memory
  form (Data1 to Data3 little-endian). `str` is a `u16` length and that many bytes, with no
  terminator; the length `0xFFFF` means "no string". `bytes` is a `u32` length and that many bytes.
  `rest` is everything up to the end of the datagram. `Name[n]` is n of the structure `Name`, where
  n is the count field just before it. Any other name is a structure defined in these documents.
- A size is a number of bytes, or `var` for a field whose length is carried in the data. Fields
  after the first `var` field have no fixed offset (`—`).
- "Size: N bytes" is the exact size. "Size: at least N bytes" counts the fixed-size fields only.
  The "Minimum: M bytes" line after it is the smallest one a decoder accepts: N, plus 2 for each
  `str` (an absent string), 4 for each `bytes` (an empty one), the minimum of each variable
  structure, and 0 for each array. A `rest` adds 0, or K when its meaning says "at least K bytes".
- Every decoder checks bounds and refuses a datagram that is short, has trailing bytes, has a
  count over its limit, or has an unknown type. A refused datagram is dropped whole, counted, and
  never reaches the game (DESIGN §9).

## Envelope

### Rules

- **Where it is used.** On the LAN (the B8 module today), session messages travel bare, one per
  UDP datagram, as below. Over the Internet (N5 onward), each one travels in one `EnvelopeData`,
  split into fragments when it is larger than one envelope can carry. The envelope does not change
  the bytes of the session message it carries.
- **Maximum datagram size.** An envelope is at most **1200 bytes** on the wire, header included.
  The fragment payload is therefore at most 1168 bytes. 1200 fits the IPv6 minimum MTU of 1280,
  with room for the IP and UDP headers and one layer of tunnelling. No envelope relies on IP
  fragmentation, which NATs and firewalls often drop. The relay forwards envelopes unchanged, so
  the limit holds on relayed paths too.
- **Peers and sessions.** `sessionId` is the 64-bit id the lobby gave the session (`LOBBY_API.md`).
  `source` and `destination` are the peer ids the lobby gave the two machines in their tickets: the
  host is peer 1, and joiners get 2 and up. These ids are the transport's own. They are not the
  DirectPlay peer numbers inside the session messages. A receiver drops an envelope whose
  `sessionId` or `destination` is not its own, or whose `source` is not the peer it expects on that
  path.
- **Sequence numbers.** Each direction of a path (source to destination) has two counters, both
  starting at 1. Reliable `EnvelopeData` envelopes use one. Every other envelope uses the other,
  including unreliable data and every control kind. All fragments of one message carry the same
  number. Numbers wrap modulo 2³², and "later" is serial-number arithmetic (RFC 1982): a is later
  than b when `(a - b) mod 2³²` is in `1 .. 2³¹ - 1`.
- **Unreliable delivery** (flag `kReliable` clear). There is no order and no retransmission. The
  receiver drops a duplicate, one whose sequence it has already taken within the last 1024 numbers,
  and it drops anything older than that window. The session messages of version 1 are sent this
  way (but see "Fragmentation"), because the session layer has its own channel: `Reliable` and `Ack` below, 200 ms
  retransmission. Two reliability layers on one message would only add delay.
- **Reliable delivery** (flag `kReliable` set). Each message is delivered once, in sequence order
  for its direction. The receiver acknowledges with `EnvelopeAck`: the highest number it took in
  order, and a mask of the 32 numbers after it. It sends an ack at once for a message that arrives
  out of order, and otherwise within 50 ms. The sender retransmits an unacknowledged message, all
  its fragments, after the retransmission timeout. The timeout is the larger of 200 ms and twice
  the smoothed round-trip time, it doubles at each retry up to 2 s, and after 10 s without an ack
  the path is dead. At most 256 reliable messages may be unacknowledged per direction. Past that,
  the send call fails with the transport's would-block result. Version 1 uses reliable envelopes
  for `EnvelopeClose`, and N5 may use them for a fragmented session message. The mode exists so that a later session version can drop its own
  channel without changing the envelope.
- **Fragmentation.** A message larger than 1168 bytes is sent as `fragmentCount` envelopes of 1168
  bytes each, the last one shorter. All of them carry the same `sequence` and flags, with
  `fragmentIndex` 0 to `fragmentCount - 1`. Limits: `fragmentCount` is 1 to 64, so a message is at
  most 74,752 bytes. That covers the largest session datagram, 65,507 bytes (`Wire.h`,
  `kMaxDatagram`). An unfragmented envelope has index 0 and count 1. The receiver reassembles each
  `(source, sequence, reliable)` separately. It holds at most 16 incomplete messages and 256 KB per
  source, and it gives one up 5 s after its first fragment arrived. Fragments that disagree on
  `fragmentCount`, or an index at or past the count, are dropped. An unreliable message with a
  lost fragment is lost whole. That matters for the session's large messages: a `Reliable`
  datagram carrying a big `JoinAccept` or `Data` can need up to 57 fragments, and with
  `kReliable` clear one lost fragment loses all of it, until the session channel retransmits the
  whole datagram 200 ms later. N5 may set `kReliable` on a message that needs more than one
  fragment; a receiver handles either.
- **Keepalive and liveness.** A machine that has sent nothing on a path for 1 s sends an
  `EnvelopePing`. The other side answers with an `EnvelopePong`, which gives both sides a
  round-trip time. A path on which nothing has arrived for 10 s is dead. The transport closes it,
  and the session layer sees it as the peer leaving, which is the 10 s peer timeout the dplay
  module already applies. A ping each second also holds open the NAT bindings on the path, which
  usually last 30 s or more for UDP.
- **Closing.** A machine that leaves sends `EnvelopeClose` (reliable) to each peer, after the
  session's own `Leave`. The receiver closes the path without waiting for the timeout.
- **Relayed and direct are the same.** A relayed envelope is byte for byte what the direct one
  would be. The relay forwards envelopes unchanged and adds nothing (`RENDEZVOUS.md`, "Relay").
  The receiver accepts an envelope from the peer's direct address, the one learned in the punch,
  or from the relay's address, and from no other address. It answers a ping on the path the ping
  came in on and sends everything else on the path it chose (`RENDEZVOUS.md`, "The punch
  sequence", step 6).
- **Versions.** A receiver drops an envelope whose `version` it does not know, and counts it. A
  version change is a new row in "Envelope versions". The lobby keeps the clients of one version
  together (`LOBBY_API.md`, `protocol`).

### Messages

| Name | Kind | Version | Direction | Purpose |
| --- | --- | --- | --- | --- |
| `EnvelopeData` | 1 | 1 | peer → peer | One session message, or one fragment of it |
| `EnvelopeAck` | 2 | 1 | peer → peer | Acknowledges reliable envelopes |
| `EnvelopePing` | 3 | 1 | peer → peer | Keepalive and round-trip probe |
| `EnvelopePong` | 4 | 1 | peer → peer | Answer to a ping |
| `EnvelopeClose` | 5 | 1 | peer → peer | The sender left the session |

### `EnvelopeHeader`

Structure. The first 32 bytes of every envelope.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 4 | magic | u32 | `0x454E4653`, the bytes `53 46 4E 45` ("SFNE") |
| 4 | 1 | version | u8 | 1 |
| 5 | 1 | kind | u8 | 1 to 5, the table above |
| 6 | 1 | flags | u8 | bit 0 `kReliable`; bit 1 `kFragment` (set when `fragmentCount` > 1); other bits 0 |
| 7 | 1 | reserved | u8 | 0; a receiver ignores it |
| 8 | 8 | sessionId | u64 | The lobby's session id |
| 16 | 4 | source | u32 | The sender's peer id |
| 20 | 4 | destination | u32 | The receiver's peer id |
| 24 | 4 | sequence | u32 | The counter for this direction, as "Sequence numbers" says |
| 28 | 2 | fragmentIndex | u16 | 0 to `fragmentCount - 1` |
| 30 | 2 | fragmentCount | u16 | 1 to 64; 1 for every kind but `EnvelopeData` |

Size: 32 bytes.

### `EnvelopeData`

Kind 1. One session message, or one fragment of one.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 32 | header | EnvelopeHeader | kind 1 |
| 32 | var | payload | rest | At least 1 byte and at most 1168: the session message, or its fragment `fragmentIndex` |

Size: at least 32 bytes. Minimum: 33 bytes.

### `EnvelopeAck`

Kind 2. Unreliable.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 32 | header | EnvelopeHeader | kind 2 |
| 32 | 4 | cumulative | u32 | The highest reliable sequence taken in order (0: none yet) |
| 36 | 4 | mask | u32 | Bit i set: `cumulative + 1 + i` was also taken |

Size: 40 bytes.

### `EnvelopePing`

Kind 3. Unreliable.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 32 | header | EnvelopeHeader | kind 3 |
| 32 | 4 | nonce | u32 | Chosen by the sender, returned in the pong |
| 36 | 4 | sentMs | u32 | The sender's clock in milliseconds, wrapping; only the sender reads it |

Size: 40 bytes.

### `EnvelopePong`

Kind 4. Unreliable.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 32 | header | EnvelopeHeader | kind 4 |
| 32 | 4 | nonce | u32 | The ping's nonce |
| 36 | 4 | sentMs | u32 | The ping's `sentMs`, unchanged |

Size: 40 bytes.

### `EnvelopeClose`

Kind 5. Reliable.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 32 | header | EnvelopeHeader | kind 5 |
| 32 | 4 | reason | u32 | 0 left the session; 1 the session closed; 2 protocol error |

Size: 36 bytes.

### Envelope versions

| Version | Date | Package | Change |
| ---: | --- | --- | --- |
| 1 | 2026-11-06 | N1 | First version: header, data, ack, ping, pong, close; 1200-byte limit, 64 fragments |

## Session messages

The datagrams of `src/hle/dplay/Wire.h`, written down from `Wire.cpp` and `WireTest.cpp`. The
behaviour they carry is in `src/hle/dplay/README.md`: joins, the mesh, 200 ms retransmission on
the channel, 1 s keepalives, the 10 s peer timeout and host migration. Limits enforced by the
decoders: 1024 peers in a join accept, 4096 entities, 4096 members in a group. A game message is at
most 60,000 bytes (`kMaxPayload`), and a datagram at most 65,507 (`kMaxDatagram`). The session
key in `PeerHeader` is the 32-bit XOR fold of the session's instance GUID. A fold that comes to 0
is replaced by 1.

**Over the Internet** (N5), the messages keep these layouts. Two fields change meaning, which N5
implements and tests:

- `PeerInfo.address` holds the peer's envelope peer id, and `PeerInfo.port` is 0. A machine
  reaches that peer through the Rendezvous by the name `RENDEZVOUS.md` gives it. A raw IPv4 address
  would be useless through NAT.
- `EnumRequest` and `EnumReply` are not broadcast. The game's session list comes from the lobby
  (`LOBBY_API.md`, `SessionInfo`), and `JoinRequest` is the first message sent on the path the
  rendezvous opened.

### Messages

| Name | Type | Version | Direction | Purpose |
| --- | --- | --- | --- | --- |
| `EnumRequest` | 1 | 1 | anyone → host's enumeration port | Ask for the host's session |
| `EnumReply` | 2 | 1 | host → enumerator | The session, without its password |
| `JoinRequest` | 3 | 1 | joiner → host | Join a session by instance |
| `JoinReject` | 4 | 1 | host → joiner | The join failed, with the DirectPlay error |
| `Reliable` | 5 | 1 | peer → peer | One inner message on the sequenced channel |
| `Ack` | 6 | 1 | peer → peer | The highest channel sequence taken in order |
| `Unreliable` | 7 | 1 | peer → peer | One game message, not acknowledged |
| `Keepalive` | 8 | 1 | peer → peer | Nothing sent for a second |
| `Leave` | 9 | 1 | peer → peer | The sender closed the session |

Inner messages, carried by `Reliable` after its sequence number:

| Name | Type | Version | Purpose |
| --- | --- | --- | --- |
| `JoinAccept` | 1 | 1 | The joiner's peer number, the session, the other machines and the name table |
| `PeerAdd` | 2 | 1 | A machine joined |
| `PeerRemove` | 3 | 1 | A machine left |
| `EntityAdd` | 4 | 1 | A player or group was created |
| `EntityRemove` | 5 | 1 | A player or group was destroyed |
| `SetData` | 6 | 1 | A player's or group's remote data changed |
| `SetName` | 7 | 1 | A player's or group's name changed |
| `MemberAdd` | 8 | 1 | A player was added to a group |
| `MemberRemove` | 9 | 1 | A player was removed from a group |
| `SessionDesc` | 10 | 1 | The session description changed |
| `Data` | 11 | 1 | A guaranteed game message |

### `Header`

Structure. The first 8 bytes of every session datagram.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 4 | magic | u32 | `0x50444653`, the bytes `53 46 44 50` ("SFDP") |
| 4 | 1 | version | u8 | 1 |
| 5 | 1 | type | u8 | 1 to 9 |
| 6 | 2 | reserved | u16 | 0 |

Size: 8 bytes.

### `PeerHeader`

Structure. Datagrams between members of a session (types 5 to 9).

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 8 | header | Header | |
| 8 | 4 | sessionKey | u32 | The fold of the instance GUID |
| 12 | 4 | sender | u32 | The sender's DirectPlay peer number |

Size: 16 bytes.

### `SessionDescription`

Structure. `DPSESSIONDESC2` without `dwSize` and the reserved words.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 4 | flags | u32 | `DPSESSION_*`; in an enumeration reply `DPSESSION_PASSWORDREQUIRED` is set when there is a password |
| 4 | 16 | instance | guid | The session instance |
| 20 | 16 | application | guid | The game's application GUID |
| 36 | 4 | maxPlayers | u32 | 0: no limit |
| 40 | 4 | currentPlayers | u32 | |
| 44 | 4 | user1 | u32 | `dwUser1` to `dwUser4`, the game's own words |
| 48 | 4 | user2 | u32 | |
| 52 | 4 | user3 | u32 | |
| 56 | 4 | user4 | u32 | |
| 60 | var | name | str | The session name |
| — | var | password | str | Absent (`0xFFFF`) in an enumeration reply |

Size: at least 60 bytes. Minimum: 64 bytes.

### `Name`

Structure. `DPNAME`.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | var | shortName | str | |
| — | var | longName | str | |

Size: at least 0 bytes. Minimum: 4 bytes.

### `PeerInfo`

Structure. A machine in the session.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 4 | id | u32 | Its DirectPlay peer number |
| 4 | 4 | address | u32 | LAN: its IPv4 address (0: the address the host is reached at). Internet: its envelope peer id |
| 8 | 2 | port | u16 | LAN: its UDP port. Internet: 0 |

Size: 10 bytes.

### `Entity`

Structure. A player or group in the name table.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 4 | id | u32 | The owner's peer number in the high 16 bits, its counter in the low 16 |
| 4 | 1 | group | u8 | 0 player, 1 group; anything else is refused |
| 5 | 4 | flags | u32 | `DPPLAYER_*` or `DPGROUP_*` |
| 9 | 4 | owner | u32 | The peer that created it |
| 13 | var | name | Name | |
| — | var | data | bytes | The remote data every machine sees |
| — | 2 | memberCount | u16 | Groups: at most 4096; players: 0 |
| — | var | members | u32[n] | The members' ids |

Size: at least 15 bytes. Minimum: 23 bytes.

### `EnumRequest`

Type 1.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 8 | header | Header | type 1 |
| 8 | 4 | requestId | u32 | Echoed in the reply |
| 12 | 16 | application | guid | Null: any application |

Size: 28 bytes.

### `EnumReply`

Type 2.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 8 | header | Header | type 2 |
| 8 | 4 | requestId | u32 | The request's |
| 12 | var | desc | SessionDescription | Without its password |

Size: at least 12 bytes. Minimum: 76 bytes.

### `JoinRequest`

Type 3.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 8 | header | Header | type 3 |
| 8 | 16 | instance | guid | The session to join |
| 24 | var | password | str | Absent: none given. Plaintext; see `src/hle/dplay/README.md`, "Known differences" |

Size: at least 24 bytes. Minimum: 26 bytes.

### `JoinReject`

Type 4.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 8 | header | Header | type 4 |
| 8 | 16 | instance | guid | The session asked for |
| 24 | 4 | result | u32 | The `HRESULT` the joiner's `Open` returns (`DPERR_*`) |

Size: 28 bytes.

### `Reliable`

Type 5. Retransmitted every 200 ms until acknowledged.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 16 | header | PeerHeader | type 5 |
| 16 | 4 | seq | u32 | The channel's sequence number to this peer |
| 20 | var | inner | rest | One inner message: at least 1 byte, its type; the decoder refuses an empty one |

Size: at least 20 bytes. Minimum: 21 bytes.

### `Ack`

Type 6.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 16 | header | PeerHeader | type 6 |
| 16 | 4 | cumulative | u32 | The highest channel sequence taken in order |

Size: 20 bytes.

### `Unreliable`

Type 7.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 16 | header | PeerHeader | type 7 |
| 16 | 4 | from | u32 | The sending player's id |
| 20 | 4 | to | u32 | The player or group sent to (0: all players) |
| 24 | var | payload | rest | The game's bytes |

Size: at least 24 bytes. Minimum: 24 bytes.

### `Keepalive`

Type 8.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 16 | header | PeerHeader | type 8 |

Size: 16 bytes.

### `Leave`

Type 9.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 16 | header | PeerHeader | type 9 |

Size: 16 bytes.

### `JoinAccept`

Inner type 1.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 1 | type | u8 | 1 |
| 1 | 4 | yourPeer | u32 | The joiner's peer number |
| 5 | 4 | nextPeer | u32 | The next number the host will give |
| 9 | var | desc | SessionDescription | With its password |
| — | 2 | peerCount | u16 | At most 1024: every machine but the host and the joiner |
| — | var | peers | PeerInfo[n] | |
| — | 2 | entityCount | u16 | At most 4096 |
| — | var | entities | Entity[n] | The name table |

Size: at least 13 bytes. Minimum: 77 bytes.

### `PeerAdd`

Inner type 2.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 1 | type | u8 | 2 |
| 1 | 10 | peer | PeerInfo | |

Size: 11 bytes.

### `PeerRemove`

Inner type 3.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 1 | type | u8 | 3 |
| 1 | 4 | peer | u32 | Its peer number |

Size: 5 bytes.

### `EntityAdd`

Inner type 4.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 1 | type | u8 | 4 |
| 1 | var | entity | Entity | Must be the sender's own, with an id from its range |

Size: at least 1 byte. Minimum: 24 bytes.

### `EntityRemove`

Inner type 5.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 1 | type | u8 | 5 |
| 1 | 4 | id | u32 | |

Size: 5 bytes.

### `SetData`

Inner type 6.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 1 | type | u8 | 6 |
| 1 | 4 | id | u32 | |
| 5 | var | data | bytes | |

Size: at least 5 bytes. Minimum: 9 bytes.

### `SetName`

Inner type 7.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 1 | type | u8 | 7 |
| 1 | 4 | id | u32 | |
| 5 | var | name | Name | |

Size: at least 5 bytes. Minimum: 9 bytes.

### `MemberAdd`

Inner type 8.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 1 | type | u8 | 8 |
| 1 | 4 | group | u32 | |
| 5 | 4 | player | u32 | |

Size: 9 bytes.

### `MemberRemove`

Inner type 9.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 1 | type | u8 | 9 |
| 1 | 4 | group | u32 | |
| 5 | 4 | player | u32 | |

Size: 9 bytes.

### `SessionDesc`

Inner type 10.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 1 | type | u8 | 10 |
| 1 | var | desc | SessionDescription | With its password |

Size: at least 1 byte. Minimum: 65 bytes.

### `Data`

Inner type 11.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 1 | type | u8 | 11 |
| 1 | 4 | from | u32 | The sending player's id |
| 5 | 4 | to | u32 | The player or group sent to |
| 9 | var | payload | rest | The game's bytes |

Size: at least 9 bytes. Minimum: 9 bytes.

### Session message versions

| Version | Date | Package | Change |
| ---: | --- | --- | --- |
| 1 | 2026-11-04 | B8 | The LAN protocol: nine datagrams, eleven inner messages (#38) |

## Adding a message

As `docs/CONVENTIONS.md` §8 says: the message goes here first, with its fields, its version and
the behaviour it carries; `tests/test_protocol_docs.py` must still pass. Then come the encoder and
decoder with bounds checks, and a doctest with hand-written bytes each way plus one malformed case.
A message added to a version that has shipped makes a new version row; a row is never edited. The
document is a contract: the integrator merges it before any code consumes it.
