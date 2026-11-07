# Lobby HTTPS API

The lobby gives native clients the server list, takes session registrations and heartbeats, and
issues the rendezvous tickets (DESIGN §5.12, §6.7). It is a small HTTPS service on the owner's
server. The session list lives in memory with a short time to live. There are no accounts, no
database and no disk state, and a restart empties the list. Hosts re-register within one
heartbeat, because their heartbeat then gets `not_found`. That is the whole of its persistence, by
design: it fits the one small VPS that DESIGN §10 and §11 allow, under 5 USD a month. The UDP side
(binding, hole punching, relay) is `RENDEZVOUS.md`. The envelope is `PROTOCOL.md`.

## Conventions

- **Transport.** HTTPS only, TLS 1.2 or later, on port 443 of the host the client's configuration
  names. The port ships that address and the player may edit it (DESIGN §5.12). The TLS
  certificate lives in the host's environment, never in the repository (DESIGN §9).
- **Bodies.** UTF-8 JSON, `Content-Type: application/json`, at most 4 KB per request. Unknown
  fields in a request are ignored, so a newer client can talk to an older lobby. A response may
  gain fields, which clients ignore.
- **Ids.** A session id is 16 lowercase hexadecimal digits (the 64-bit `sessionId` of the envelope
  and the ticket). A peer id is 8 lowercase hexadecimal digits. GUIDs are text in the registry
  form, `{0AB1C531-4745-11D1-A7A1-0000F803ABFC}`. A ticket is its 36 bytes (`RENDEZVOUS.md`,
  `Ticket`) in unpadded base64url, 48 characters.
- **Versions.** The path carries the API version (`/v1`). Every request body (registration,
  heartbeat, ticket) and the list query carry `protocol`, the envelope version the client speaks
  (`PROTOCOL.md`). Removal has no body; its path names the session, whose version is known. The lobby lists, and
  issues tickets for, only sessions whose `protocol` equals the client's. A client with a
  `protocol` the lobby does not know gets `unsupported_version`. An incompatible API change is a
  new path prefix, served next to the old one for as long as the owner chooses.
- **Client timeouts.** The client gives a connection 3 s and a whole request 8 s. If the lobby is
  unreachable, the game browser shows the original's "no games" state within 10 s, never a hang
  (DESIGN §5.12). The client asks for the list again no sooner than `next_poll_s`.
- **Host secret.** Registration returns a random 128-bit `host_secret` (base64url). Heartbeat and
  removal must present it. The lobby compares it in constant time, never logs it, and keeps it
  only with the session.

## Endpoints

| Method | Path | Request | Response | Since |
| --- | --- | --- | --- | --- |
| GET | `/v1/health` | — | `Health` | 1 |
| GET | `/v1/sessions?protocol=<n>&application=<guid>` | — | `SessionList` | 1 |
| POST | `/v1/sessions` | `RegisterRequest` | `RegisterResponse` | 1 |
| PUT | `/v1/sessions/<session>` | `HeartbeatRequest` | `HeartbeatResponse` | 1 |
| DELETE | `/v1/sessions/<session>` | — | — | 1 |
| POST | `/v1/sessions/<session>/tickets` | `TicketRequest` | `TicketResponse` | 1 |

- **Health.** `GET /v1/health` answers `200` with `Health` while the service runs. The host's own
  monitor polls it every minute, and a failure sends the owner one message (DESIGN §10). It reads
  nothing and writes nothing.
- **Server list.** `GET /v1/sessions` answers `200` with every listed session of that `protocol`
  and `application` (the game's application GUID), newest first, at most 200. A session the host
  marked full or closed to joining is still listed, with its flags, as DirectPlay listed it. The
  list never contains a host's address. A client reaches a host only through a ticket and the
  rendezvous.
- **Registration.** `POST /v1/sessions` creates a session and answers `201` with
  `RegisterResponse`. The response holds the session id, the host's ticket (peer 1), the host
  secret, the rendezvous address and the heartbeat period. A host registers when the game creates
  a session for the Internet connection. It binds at the rendezvous at once
  (`RENDEZVOUS.md`, "Bindings").
- **Heartbeat.** `PUT /v1/sessions/<session>` every `heartbeat_s` (15 s). It carries the host
  secret and the session's current description, so a change of name, player count or flags reaches
  the list within one heartbeat. It answers `200` with `HeartbeatResponse`. A session with no
  heartbeat for `ttl_s` (45 s) is removed. A heartbeat for an unknown session answers `not_found`,
  and the host registers again, which gives a new session id. Players already in the session keep
  their paths. Only new joiners see the new id.
- **Removal.** `DELETE /v1/sessions/<session>` with the header `X-Host-Secret: <host_secret>`
  answers `204`. The host sends it when it closes the session. A host that crashes is removed by
  the time to live. Removal also drops the session's rendezvous bindings.
- **Ticket.** `POST /v1/sessions/<session>/tickets` answers `201` with a new peer id and its
  ticket, valid for 10 minutes, and the rendezvous address. The joiner binds with it and then
  connects to the host by name (`RENDEZVOUS.md`, "Names"). The password, if any, is not checked
  here: the host checks it in `JoinRequest` (`PROTOCOL.md`), as on the LAN. With `renew`, the lobby
  reissues a ticket for the same peer id if the old ticket's `mac` is right and the session still
  exists. This is how a machine whose binding lapsed binds again (`RENDEZVOUS.md`, "Tickets").
  Peer ids are given in order from 2. A session gets at most 64 tickets in 10 minutes, renewals
  included; past that it answers `session_full`.

## Objects

Field tables: name, JSON type, whether a request must carry it, meaning. JSON types are `string`,
`integer`, `boolean`, `object`, an object name below, or `X[]` for an array of `X`.

### `Health`

| Field | JSON type | Required | Meaning |
| --- | --- | --- | --- |
| status | string | yes | `"ok"` |
| api | integer | yes | 1, the highest API version served |
| protocols | integer[] | yes | The envelope versions served, `[1]` |
| sessions | integer | yes | Sessions listed (the rendezvous: bindings held) |
| uptime_s | integer | yes | Seconds since the service started |

### `SessionInfo`

| Field | JSON type | Required | Meaning |
| --- | --- | --- | --- |
| session_id | string | yes | 16 hex digits |
| protocol | integer | yes | Its envelope version |
| application | string | yes | The game's application GUID |
| name | string | yes | The session name, 1 to 63 bytes of UTF-8 |
| flags | integer | yes | `DPSESSION_*` as the host set them, with `DPSESSION_PASSWORDREQUIRED` if it has a password |
| max_players | integer | yes | 0 to 64; 0: no limit |
| current_players | integer | yes | 0 to `max_players` |
| user | integer[] | yes | Exactly four unsigned 32-bit words, `dwUser1` to `dwUser4` |
| age_s | integer | yes | Seconds since registration |

### `SessionList`

| Field | JSON type | Required | Meaning |
| --- | --- | --- | --- |
| sessions | SessionInfo[] | yes | |
| next_poll_s | integer | yes | 5: the soonest the client asks again |

### `ServiceAddress`

| Field | JSON type | Required | Meaning |
| --- | --- | --- | --- |
| host | string | yes | A host name or dotted quad |
| port | integer | yes | 47710 |

### `RegisterRequest`

| Field | JSON type | Required | Meaning |
| --- | --- | --- | --- |
| protocol | integer | yes | The client's envelope version |
| application | string | yes | The game's application GUID |
| name | string | yes | As in `SessionInfo` |
| flags | integer | yes | |
| max_players | integer | yes | |
| current_players | integer | yes | |
| user | integer[] | yes | Four words |
| password_required | boolean | yes | The lobby adds `DPSESSION_PASSWORDREQUIRED` to the listed flags; the password never leaves the host |

### `RegisterResponse`

| Field | JSON type | Required | Meaning |
| --- | --- | --- | --- |
| session_id | string | yes | |
| peer_id | string | yes | `"00000001"` |
| host_secret | string | yes | For heartbeat and removal |
| ticket | string | yes | The host's ticket |
| rendezvous | ServiceAddress | yes | Where to bind |
| heartbeat_s | integer | yes | 15 |
| ttl_s | integer | yes | 45 |

### `HeartbeatRequest`

| Field | JSON type | Required | Meaning |
| --- | --- | --- | --- |
| protocol | integer | yes | Must equal the session's; otherwise `unsupported_version` |
| host_secret | string | yes | |
| name | string | yes | |
| flags | integer | yes | |
| max_players | integer | yes | |
| current_players | integer | yes | |
| user | integer[] | yes | |
| password_required | boolean | yes | |

### `HeartbeatResponse`

| Field | JSON type | Required | Meaning |
| --- | --- | --- | --- |
| heartbeat_s | integer | yes | 15; the lobby may raise it under load |
| ttl_s | integer | yes | 45 |

### `TicketRequest`

| Field | JSON type | Required | Meaning |
| --- | --- | --- | --- |
| protocol | integer | yes | Must equal the session's |
| renew | TicketRenewal | no | Present to reissue a ticket for a peer id this machine already holds |

### `TicketRenewal`

| Field | JSON type | Required | Meaning |
| --- | --- | --- | --- |
| peer_id | string | yes | |
| ticket | string | yes | The machine's last ticket for this session, expired or not |

### `TicketResponse`

| Field | JSON type | Required | Meaning |
| --- | --- | --- | --- |
| peer_id | string | yes | |
| ticket | string | yes | |
| expires_s | integer | yes | 600 |
| rendezvous | ServiceAddress | yes | |

### `ErrorResponse`

The body of every error.

| Field | JSON type | Required | Meaning |
| --- | --- | --- | --- |
| error | ErrorBody | yes | |

### `ErrorBody`

| Field | JSON type | Required | Meaning |
| --- | --- | --- | --- |
| code | string | yes | A code from "Errors" |
| message | string | yes | One English sentence for the log on the player's machine; never shown as is |
| retry_after_s | integer | no | With `rate_limited` and `unavailable` |

## Errors

| Code | HTTP | When |
| --- | ---: | --- |
| `bad_request` | 400 | Malformed JSON, a missing or out-of-range field, a body over 4 KB, a bad id or ticket encoding |
| `unsupported_version` | 400 | The client's `protocol` is not served |
| `forbidden` | 403 | A wrong host secret, a renewal whose ticket is not this session's, or a blocked address |
| `not_found` | 404 | No such session, or no such path |
| `session_full` | 409 | The session's ticket budget for the last 10 minutes is spent |
| `rate_limited` | 429 | Over a limit below; `Retry-After` header and `retry_after_s` |
| `internal` | 500 | A defect in the lobby |
| `unavailable` | 503 | The session table is full (2048), or the service is shutting down |

A client treats any other status, a TLS failure or a timeout as "lobby unreachable".

## Rate limits

Per source address, as token buckets. Over a limit the request answers `rate_limited` and is not
processed.

| Requests | Limit |
| --- | --- |
| `GET /v1/sessions` | 30 a minute |
| `POST /v1/sessions` | 6 a minute |
| `PUT /v1/sessions/<session>` | 10 a minute |
| `DELETE /v1/sessions/<session>` | 6 a minute |
| `POST /v1/sessions/<session>/tickets` | 30 a minute |
| `GET /v1/health` | unlimited from 127.0.0.1, 6 a minute from anywhere else |
| All requests together | 120 a minute |

The owner blocks an address in the service's configuration. Its requests get `forbidden` and
nothing else.

## What is logged

DESIGN §9 says nothing identifying is kept beyond what a session needs.

- **Per request:** the time, the method, the path pattern (`/v1/sessions/<session>`, with the id
  left out), the status and the duration. No body, no session name, no host secret, no ticket and
  no address.
- **Per event:** a rate-limit or block event, with the source address. A session registered or
  removed, with its session id and player count. The session id is a random number the lobby
  chose; it names no machine and holds no address.
- **Kept:** logs rotate daily and are deleted after 7 days (DESIGN §10). The source addresses held
  in memory for rate limiting are dropped a minute after their last request. The session list,
  with its names, lives only in memory, and only while the session does.

## Versions

| Version | Date | Package | Change |
| ---: | --- | --- | --- |
| 1 | 2026-11-06 | N1 | First version: health, list, register, heartbeat, remove, tickets |
