# sfcop-relay

The UDP service on port 47710 (`src/net/RENDEZVOUS.md`): the table of bindings, the introduction
of two peers, and the relay that forwards envelopes between machines of one session that each
hold a ticket the lobby issued. One small process, state in memory only: a restart forgets every
binding and clients bind again. No database, no disk, no accounts.

## What it does

- **Bind / BindReply / Unbind.** Checks the ticket (`HMAC-SHA-256` over the first 20 bytes with the
  32-byte key shared with the lobby, first 16 bytes compared; the role must agree with the peer
  id), keeps `(session, peer) -> address` for 45 s, answers with the address it saw. A new
  binding needs an unexpired ticket; a refresh from the binding's own address does not. A bind
  from another address moves the binding only with an unexpired ticket. At most 8192 bindings.
- **Connect / PeerCandidates.** Introduces two bound peers of one session: each gets the other's
  reflexive address first, then its local candidates, with one nonzero punch id, sent twice 100 ms
  apart. A repeated `Connect` (or the other side's) within 3 s reuses the punch id.
- **Relay.** An envelope (`SFNE`) is forwarded byte for byte to the destination peer's binding
  when its source is bound at the datagram's source address and its destination is bound in the
  same session. Everything else is dropped with no answer. The relay reads the 32-byte header
  only, never the payload; a datagram over 1200 bytes, a short one or an unknown version is
  dropped.
- **Limits.** Per binding as the source: 1000 datagrams and 512 KB a second, a token bucket with
  a one-second burst. Per source address: `Bind` 30 a minute, `Connect` 60, all rendezvous
  messages 120, each a bucket refilled evenly over the minute. Past a type's limit the answer is
  `kRateLimited`; past the total, none (so a flood cannot be reflected). At most 65,536 sources
  are tracked.
- **Block list.** One IPv4 address per line in the file `--blocklist` names; `SIGHUP` reloads it.
  Blocked datagrams are dropped unanswered.
- **Loopback HTTP on 127.0.0.1:47711.** `GET /v1/health` is the lobby's `Health` object
  (`sessions` is the number of bindings). `DELETE /v1/bindings/<16 hex digits>` is for the
  lobby: when it removes a session it calls this, and the relay drops that session's bindings on
  its next turn (without the call they would expire within 45 s anyway). Not reachable from
  outside the host.

## The call the lobby makes (session removal)

Loopback only (`127.0.0.1`, the health port). The text N3 codes against:

```
DELETE /v1/bindings/<session>
```

- `<session>`: the lobby's session id, exactly 16 lowercase hexadecimal digits. No body, no
  request headers required.
- `204 No Content`, empty body: the removal is queued; the relay drops every binding of that
  session on its next turn (within 20 ms). It is the answer whether or not the session had
  bindings, so the call is idempotent and reveals nothing.
- `400 Bad Request`: `<session>` is not 16 lowercase hexadecimal digits (or the request line is
  malformed). `405 Method Not Allowed`: any method but `DELETE` on this path. `404 Not Found`: any
  other path. Errors have empty bodies.
- The connection is closed after each answer. Nothing else is accepted from a client that sends
  nothing for a second.

`GET /v1/health` answers `200` with `{"status":"ok","api":1,"protocols":[1],"sessions":<bindings>,"uptime_s":<n>}`.

## Configuration names and ports

Both services read the shared ticket key from the host's environment file
(`/etc/sfcop/sfcop.env`, mode 0600) as **`SFCOP_TICKET_KEY`**, 64 hexadecimal digits. The relay's
own settings are in `/etc/sfcop/relay.env` (`server/deploy/relay.env.example`): `SFCOP_RELAY_PORT`
(UDP, default **47710**), `SFCOP_HEALTH_LISTEN` (loopback HTTP, **127.0.0.1:47711**; the host's
monitor reads the same name), `SFCOP_LOG_LEVEL`. Defaults are in `Config.h` only. The lobby's
`lobby.env` carries `SFCOP_RELAY_PORT` and `SFCOP_RELAY_HEALTH_PORT` as the numbers it tells
clients and calls; keep them equal to the relay's. Options (`--port`, `--health-port`, `--bind`,
`--health-bind`, `--blocklist FILE`, `--log-level`) override the environment.

## Refusals worth knowing

- A `Bind` whose candidates include `0.0.0.0`, `255.255.255.255`, a multicast address (224/4) or
  port 0 is dropped whole and counted as malformed, with no answer: RENDEZVOUS.md has no result
  code for it. Loopback candidates are allowed (two clients on one machine, the container test).
- `Connect` for a session with no bindings is answered `kUnknownSession`; for a session that has
  some, from a machine not bound at that address, `kUnknownBinding`.
- An unsupported `version` is answered `kUnsupportedVersion` for `Bind` and `Connect` only.
- Rendezvous limits and the source table are keyed by IPv4 address, not address and port. When the
  table (65,536 addresses) is full, the address idle the longest is dropped to make room.

## Logging

One line per sentence, to stderr (the unit appends it to `/var/log/sfcop/relay.log`, directory 0750 and file 0640, rotated daily
and kept 7 days). Above debug level only counts and events are written: start, stop, the
per-minute totals (bindings, datagrams and bytes forwarded, datagrams refused by reason, rate
limited), the block-list reload. **No address, session id or peer id appears above debug.** Rate
limit and block events with the source address are debug lines (`--log-level debug`); that is how
the owner finds an abuser, and debug is off by default. Payloads are never read, so never logged.

## Configuration

Environment (the host's `/etc/sfcop/sfcop.env`, mode 0600, outside the repository):
`SFCOP_TICKET_KEY` = 64 hexadecimal digits, the key the lobby signs tickets with. Options:
`--port`, `--health-port`, `--bind`, `--health-bind`, `--blocklist`, `--log-level`.

## Build and test

`server/CMakeLists.txt` adds this directory beside the lobby:

```
cmake -S server -B <build> -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build <build> -j4
ctest --test-dir <build> --output-on-failure      # sfcop_lobby_tests and sfcop_relay_tests
<build>/relay/sfcop-relay-bench                  # the state machine's forwarding rate, no socket
```

`scripts/check.sh` runs the relay's tests through `tests/test_server.py` (a no-network doctest
run that skips with a reason when no doctest source is at hand). The tests use fakes for the
socket, the clock and the random source. Dependencies: the C++ standard library and POSIX
sockets only; SHA-256 and HMAC are in `Sha256.cpp` (checked against the published vectors and
the lobby's ticket vector in `server/README.md`).

## Deploy

`server/deploy/deploy.sh` builds and installs the relay with the lobby: `/opt/sfcop/bin/sfcop-relay`,
`sfcop-relay.service` (user `sfcop`, log `/var/log/sfcop/relay.log` in a 0750 directory with
`UMask=0027`), `relay.env`, an empty `relay-blocklist.txt`, the shared logrotate rule (daily, 7
days) and `sfcop-health@relay.timer`, which runs the monitor every minute. `server/deploy/
test-container.sh` installs it on a fresh Debian and, among the lobby's checks, verifies both
units with `systemd-analyze`, runs the relay as `sfcop`, and drives it with `relay-peers.py`: two
peers and a stranger over loopback UDP, the 1000-datagram cap, the removal call and the health
endpoint.

What that container test shows and does not: the relay runs through `su`, not under systemd, and
`systemd-analyze verify` checks the unit's syntax, so its hardening options are checked for form,
not exercised; the first start on a real host is their first run.
