# UDP rendezvous, hole punching and the relay

How two native clients that the lobby has introduced find a UDP path to each other, and the relay
they fall back to when no direct path exists (DESIGN §5.11, §6.7). It is one small UDP service.
The rendezvous and the relay share a port and a process on the owner's server (DESIGN §10, §11).
The lobby's HTTPS API, which issues the tickets used here, is `LOBBY_API.md`. The envelopes the
relay forwards are `PROTOCOL.md`. The tables follow `PROTOCOL.md`, "Reading the tables":
little-endian, `u8[N]`, `var`, `—`, and the same rules for refusing a malformed datagram.

The design rests on one fact: both ends of the protocol are this project's. There is no
interoperability requirement with STUN, TURN or ICE, so the messages are only what the game needs.
`NAT_LIBRARY.md` explains the choice.

## The service

- **One UDP port, 47710**, on the host the lobby names (`LOBBY_API.md`, `ServiceAddress`). The
  first four bytes of a datagram say what it is. `53 46 52 56` ("SFRV") is a rendezvous message
  below. `53 46 4E 45` ("SFNE") is an envelope for the relay. Anything else is dropped.
- **State in memory only:** a table of bindings, at most 8192. No database, no disk and no
  accounts. A restart forgets everything. Clients notice, because their refresh gets
  `kUnknownBinding`, and they bind again with their ticket.
- **Health:** `GET /v1/health` over plain HTTP on `127.0.0.1:47711`. It returns the lobby's
  `Health` object, with `sessions` counting bindings. Only the host's own monitor reads it
  (DESIGN §10).
- **Logged:** starts and stops, counts each minute (bindings, datagrams and bytes forwarded,
  datagrams refused by reason), and rate-limit or block events. A rate-limit or block event
  carries the source address, which is kept 7 days, then deleted (DESIGN §9). No datagram
  contents are ever logged. A binding's address is held only as long as the binding.
- **The owner can block an address** in the service's configuration. Datagrams from it are
  dropped unanswered.

## Names

The Transport contract's `Rendezvous::ConnectToPeer(sessionId, timeoutMs)` takes a string. Its
forms are:

| Form | Example | Reaches |
| --- | --- | --- |
| `<session>` | `5346434f50000001` | The host of the session (peer 1) |
| `<session>:<peer>` | `5346434f50000001:00000003` | That peer of the session |

`<session>` is the lobby's 64-bit session id in 16 lowercase hexadecimal digits. `<peer>` is a peer
id in 8 lowercase hexadecimal digits. Anything else is `kInvalidArgument`. The second form gives
the session layer its mesh. Over the Internet, a `PeerInfo` carries a peer's id (`PROTOCOL.md`),
and every machine reaches every other one by name. A Dynaverse server run by this project writes
the host's name where the original wrote its IP address (`analysis/netcode/dynaverse.md`).

## Tickets

The lobby issues a ticket to every machine of a session: to the host when it registers, and to
each joiner on request. The rendezvous checks it. The ticket is opaque to clients, which pass it on
unchanged. The lobby and the rendezvous run on one host and share a 32-byte key from the host's
environment file, never from the repository (DESIGN §9).

### `Ticket`

Structure.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 8 | sessionId | u64 | The session |
| 8 | 4 | peerId | u32 | The holder's peer id: 1 for the host, 2 and up for joiners |
| 12 | 4 | expiresAt | u32 | Unix time in seconds; a new binding needs a ticket that has not expired |
| 16 | 1 | role | u8 | 1 host, 2 joiner |
| 17 | 3 | reserved | u8[3] | 0 |
| 20 | 16 | mac | u8[16] | The first 16 bytes of HMAC-SHA-256 over bytes 0 to 19 with the shared key |

Size: 36 bytes.

A ticket is valid for 10 minutes from issue. A binding outlives it. A refresh from the binding's
own address is accepted with an expired ticket whose `mac` is right. A machine whose address has
changed, after the binding expired, asks the lobby to renew its ticket (`LOBBY_API.md`, `renew`).

## Messages

| Name | Type | Version | Direction | Purpose |
| --- | --- | --- | --- | --- |
| `Bind` | 1 | 1 | client → service | Register this machine's address for its ticket, with its local candidates; also the refresh |
| `BindReply` | 2 | 1 | service → client | The result and the address the service saw |
| `Connect` | 3 | 1 | client → service | Ask to be introduced to a peer of the session |
| `PeerCandidates` | 4 | 1 | service → both peers | The other peer's candidates and the punch id |
| `Probe` | 5 | 1 | peer → peer | A hole-punching probe to one candidate |
| `ProbeReply` | 6 | 1 | peer → peer | The answer to a probe |
| `Unbind` | 7 | 1 | client → service | This machine leaves; drop its binding |
| `RvError` | 8 | 1 | service → client | A request failed |

### `RvHeader`

Structure. The first 12 bytes of every rendezvous message.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 4 | magic | u32 | `0x56524653`, the bytes `53 46 52 56` ("SFRV") |
| 4 | 1 | version | u8 | 1 |
| 5 | 1 | type | u8 | 1 to 8 |
| 6 | 2 | reserved | u16 | 0 |
| 8 | 4 | transaction | u32 | Chosen by the sender of a request, echoed in the answer; 0 in `PeerCandidates` |

Size: 12 bytes.

### `Candidate`

Structure. One address at which a machine may be reached.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 4 | address | u32 | IPv4, host-order value (`PROTOCOL.md`) |
| 4 | 2 | port | u16 | |
| 6 | 1 | kind | u8 | 1 local (an interface address of the machine); 2 reflexive (as the service saw it) |
| 7 | 1 | reserved | u8 | 0 |

Size: 8 bytes.

### `Bind`

Type 1. Sent from the socket the machine will use for envelopes, so that the binding is the NAT
mapping that envelopes will use.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 12 | header | RvHeader | type 1 |
| 12 | 36 | ticket | Ticket | |
| 48 | 1 | candidateCount | u8 | 0 to 8 |
| 49 | 1 | reserved | u8 | 0 |
| 50 | var | candidates | Candidate[n] | The machine's local candidates (kind 1) |

Size: at least 50 bytes. Minimum: 50 bytes.

### `BindReply`

Type 2.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 12 | header | RvHeader | type 2, the request's transaction |
| 12 | 4 | result | u32 | 0, or a code from "Results" |
| 16 | 8 | reflexive | Candidate | The source address of the `Bind`, kind 2 |
| 24 | 2 | refreshSeconds | u16 | 15: how often to send `Bind` again |
| 26 | 2 | reserved | u16 | 0 |

Size: 28 bytes.

### `Connect`

Type 3. From a bound machine.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 12 | header | RvHeader | type 3 |
| 12 | 8 | sessionId | u64 | |
| 20 | 4 | source | u32 | The sender's peer id; must be bound at the datagram's source address |
| 24 | 4 | target | u32 | The peer to reach; must be bound in the same session |

Size: 28 bytes.

### `PeerCandidates`

Type 4. The service sends one to each of the two peers, twice, 100 ms apart, with the same punch
id. A peer receiving a second copy ignores it.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 12 | header | RvHeader | type 4, transaction: the `Connect`'s for its sender, 0 for the target |
| 12 | 8 | sessionId | u64 | |
| 20 | 4 | peer | u32 | The other peer's id |
| 24 | 4 | punchId | u32 | Random, nonzero; ties the probes to this introduction |
| 28 | 1 | candidateCount | u8 | 1 to 9 |
| 29 | 3 | reserved | u8[3] | 0 |
| 32 | var | candidates | Candidate[n] | The other peer's reflexive candidate first, then its local ones |

Size: at least 32 bytes. Minimum: 32 bytes.

### `Probe`

Type 5. Peer to peer, to one candidate address, from the bound socket.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 12 | header | RvHeader | type 5 |
| 12 | 8 | sessionId | u64 | |
| 20 | 4 | source | u32 | The prober's peer id |
| 24 | 4 | target | u32 | The receiver's peer id |
| 28 | 4 | punchId | u32 | From `PeerCandidates` |

Size: 32 bytes.

### `ProbeReply`

Type 6. Sent to the source address of the `Probe`, unchanged except for the type and the swapped
peer ids.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 12 | header | RvHeader | type 6, the probe's transaction |
| 12 | 8 | sessionId | u64 | |
| 20 | 4 | source | u32 | The replier's peer id |
| 24 | 4 | target | u32 | The prober's peer id |
| 28 | 4 | punchId | u32 | The probe's |

Size: 32 bytes.

### `Unbind`

Type 7. Accepted only from the binding's own address. No reply.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 12 | header | RvHeader | type 7 |
| 12 | 8 | sessionId | u64 | |
| 20 | 4 | peer | u32 | |

Size: 24 bytes.

### `RvError`

Type 8. The answer to a `Bind` that cannot be sent as a `BindReply`, or to a failed `Connect`.

| Offset | Size | Field | Type | Meaning |
| ---: | ---: | --- | --- | --- |
| 0 | 12 | header | RvHeader | type 8, the request's transaction |
| 12 | 4 | result | u32 | A code from "Results" |

Size: 16 bytes.

### Results

| Code | Name | Meaning |
| ---: | --- | --- |
| 0 | `kOk` | |
| 1 | `kBadTicket` | The ticket's `mac` is wrong |
| 2 | `kTicketExpired` | A new binding with an expired ticket |
| 3 | `kUnknownSession` | No binding for the session |
| 4 | `kUnknownPeer` | The target is not bound |
| 5 | `kUnknownBinding` | The sender is not bound at this address; bind again |
| 6 | `kRateLimited` | Slow down; the request was dropped |
| 7 | `kUnsupportedVersion` | The `version` byte is not one the service speaks |
| 8 | `kFull` | The binding table is full |

## Bindings

A binding is `(sessionId, peerId) → address`, created by a `Bind` with a valid ticket. A `Bind` for
an existing binding from a different address replaces the address only with an unexpired ticket.
The client sends `Bind` again every 15 s. Doing so keeps the binding and holds the NAT mapping
open. A binding not refreshed for 45 s, or unbound, is removed. So is every binding of a session
the lobby removed: the lobby and the service share the process, or a local call between them,
which N3 and N4 decide. The host binds when it registers, and a joiner when it gets its ticket,
before any `Connect`.

## The punch sequence

`ConnectToPeer(name, timeoutMs)` on machine A, aiming at peer B. All times are from the call.

1. **0 ms.** A is bound (it bound when it got its ticket). It sends `Connect(session, A, B)` and
   repeats it every 250 ms until a `PeerCandidates` or an `RvError` arrives, for at most 500 ms.
   An `RvError` ends the call: `kUnknownSession` is `kNotFound`, `kUnknownPeer` is
   `kHostUnreachable`. If the service has answered neither by 500 ms, the call fails with
   `kTimedOut`: the service, not the peer, is out of reach, and there is no relay without it.
2. **The service introduces them.** It sends `PeerCandidates` to A, with B's reflexive and local
   candidates, and to B, with A's. Both carry the same new `punchId`. B learns of A only from
   this message.
3. **Both probe.** Each side sends a `Probe` to every candidate of the other, with the first
   candidate (the reflexive one) first, 20 ms apart. It repeats each candidate every 100 ms. A
   machine that receives a `Probe` whose session, target and `punchId` match answers with a
   `ProbeReply` to the probe's source address. It also adds that address to the candidates it is
   probing; when the reverse mapping exists, this address is the other side's real one. Probes
   with a wrong `punchId` are dropped silently.
4. **A path is chosen.** The first `ProbeReply` received selects its source address as the direct
   path. The side keeps answering probes for 500 ms more, so that the other side gets its reply
   too, then stops probing. If the two sides choose different addresses, both work, and each sends
   to the one it chose. `ConnectToPeer` returns the socket, connected to that path.
5. **Fallback to the relay at 3000 ms after the call.** If no `ProbeReply` has arrived 3000 ms
   after A called `ConnectToPeer`, A stops probing and the path is the relay. The destination
   address is the service's own, and envelopes carry B's peer id. Opening the relay takes one
   round trip: A sends an `EnvelopePing` (`PROTOCOL.md`) through it, and the `EnvelopePong` that
   comes back the same way within 1500 ms confirms the path. The worst case is therefore 4500 ms
   from the call, strictly within DESIGN §5.11's 5 s, whatever step 1 took. Without a pong by
   4500 ms, the call fails with `kHostUnreachable`. B runs the same clock from the moment its
   `PeerCandidates` arrived.
6. **When the two sides choose different paths.** One side may fall back to the relay while the
   other has a direct path, for instance when the last `ProbeReply` was lost. Both paths stay
   valid for the life of the session, and three rules make them agree:
   - A machine accepts envelopes from the other peer on either path: the direct address it learned
     in the punch, or the relay's address. From any other address it drops them.
   - It answers an `EnvelopePing` on the path the ping arrived on, so a relay open always gets its
     pong.
   - It sends everything else on the path it chose. A machine that has chosen no path yet when an
     envelope from the other peer arrives through the relay chooses the relay.
7. **A test switch** in the client forces the traversal to fail: no probes are sent, and the
   relay is chosen at once. DESIGN §5.11's test uses it to show the fallback.

A `timeoutMs` shorter than these steps cuts them short, with `kTimedOut`. A path, once chosen, is
kept for the life of the session. If it dies (`PROTOCOL.md`, 10 s with nothing received), the peer
is gone. N2 may reconnect through the relay later.

What it does and does not cover: full-cone, restricted-cone and port-restricted NATs punch
directly. A pair where either side is behind a symmetric NAT (a new port for each destination)
usually does not, and falls back to the relay. Two machines behind one NAT reach each other by
their local candidates. Port prediction and IPv6 are not in version 1. The Transport contract is
IPv4 only.

## Relay

The relay forwards envelopes unchanged (DESIGN §6.7). An envelope (magic "SFNE") arriving at the
service port is handled as follows:

1. Read the `EnvelopeHeader` only (`PROTOCOL.md`). If it is shorter than 32 bytes, or its version
   is unknown, drop it.
2. `(sessionId, source)` must be a binding whose address is the datagram's source address.
   `(sessionId, destination)` must be a binding too. Otherwise drop the datagram and count it. No
   error goes back, because an attacker must not learn which sessions exist.
3. Send the same bytes, unchanged, to the destination binding's address, from the service port.

The relay therefore forwards only between machines of one session that each hold a ticket for it
(DESIGN §9). It adds no framing. A receiver cannot tell a relayed envelope from a direct one, except
by the address it came from. The relay does not reorder, retransmit or reassemble. The envelope's
own rules cover that.

**Limits** (per binding as the source): 1000 datagrams a second and 512 KB a second, a token bucket
with a one-second burst. Past them, datagrams are dropped and the event is counted. One game
of six players exchanges a few KB a second per pair. *Estimate, to be measured by N4:* 20 relayed
pairs at 10 KB/s each is 200 KB/s, about 0.5 TB a month if they all play around the clock. The
owner's ceiling (DESIGN §11) is one small VPS, whose included traffic is usually 1 TB a month or
more. Only pairs that cannot punch use the relay.

**Rate limits on the rendezvous messages** (per source address): `Bind` 30 a minute, `Connect`
60 a minute, every rendezvous message together 120 a minute. Answers are smaller than requests,
except `PeerCandidates`. That one goes only to bound machines, so the service cannot be used to
reflect traffic at a third party.

## Versions

| Version | Date | Package | Change |
| ---: | --- | --- | --- |
| 1 | 2026-11-06 | N1 | First version: bind, connect, candidates, probes, unbind, errors; relay by binding |
