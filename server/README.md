# server/: the hosted services

One small Linux host runs the project's services (DESIGN §10, §11). Today `server/lobby/` holds
`sfcop-lobby`: the lobby HTTP API (`src/net/LOBBY_API.md`). The UDP rendezvous and the relay
(`src/net/RENDEZVOUS.md`) are `sfcop-relay` (`server/relay/`, N4), a separate process that owns the UDP
port; it adds its own unit and environment example to `server/deploy/`. The two share only the
ticket key and one loopback call.

## What it keeps, and what it does not

**Memory only.** The session list, the host secrets and the rate-limit buckets live in the process. There is no database, no account and no file state besides the log. A restart empties
the list and nothing else is lost: hosts notice by their next heartbeat (`not_found`) and register
again. The ticket key is in the environment file, so tickets issued before a restart stay valid.

## Ticket test vector

The 36-byte ticket of `src/net/RENDEZVOUS.md`, "Tickets", as the lobby issues it, for any package
that must agree with it (the relay pins these bytes). The mac was computed once outside the code:
`python3 -c "import hmac,hashlib; print(hmac.new(bytes(range(32)), bytes.fromhex('08070605040302010300000000105e5f02000000'), hashlib.sha256).digest()[:16].hex())"`.

| Part | Value |
| --- | --- |
| Key (32 bytes, bytes `0x00` to `0x1f`) | `000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f` |
| Body (bytes 0 to 19): session `0x0102030405060708`, peer 3, expiry `0x5F5E1000`, role 2, three zero bytes | `08070605040302010300000000105e5f02000000` |
| Mac (first 16 bytes of HMAC-SHA-256 over the body) | `5869d0d99c0a355d3a5e14402fed89aa` |
| Ticket (36 bytes) | `08070605040302010300000000105e5f020000005869d0d99c0a355d3a5e14402fed89aa` |

The same bytes are asserted in `server/lobby/ParsingTest.cpp` and `RelayClientTest.cpp`. The role
is 1 exactly when the peer id is 1.

## Behavior worth knowing

- **A host retrying a lost 204 gets `not_found`.** `DELETE` removes the session; a repeat finds
  none. The host treats `not_found` on a removal as done.
- **Creating twice makes two sessions.** The lobby has no client identity to merge on: each `POST`
  is a new session with its own id and secret, and an abandoned one lapses by its time to live.
  Likewise each ticket request without `renew` is a new peer (2, 3, ...); a renewal returns the
  same peer id as often as asked and takes no new one.
- A repeated heartbeat just refreshes the one session; a heartbeat after the session lapsed gets
  `not_found` and the host registers again.
- The relay call is bounded: a request handler makes at most one relay call after it answers and a
  sweep (once a second) at most five, each with a 100 ms limit that ends as soon as the status
  line arrives; the rest wait for the next turn, so a stalled relay cannot freeze the HTTP loop.

## Choices

- **Language: C++17**, as CONVENTIONS §1 says for `server/`. No dependency beyond the standard
  library and POSIX sockets: SHA-256, HMAC, base64url, JSON and the HTTP subset are in
  `server/lobby/` with tests against published vectors. doctest is the tests' only dependency (the
  pinned release of `third_party/`).
- **TLS is the host's, not the process's.** LOBBY_API.md asks for HTTPS only, and a TLS stack is a
  dependency the project does not take for a service this small. `sfcop-lobby` speaks plain HTTP
  on loopback (`SFCOP_HTTP_LISTEN`) and a TLS proxy on the host terminates HTTPS. `deploy.sh`
  installs Caddy for that when `SFCOP_LOBBY_DOMAIN` is set (certificate issued and renewed by
  Caddy, kept on the host). The lobby takes the client address for its per-address limits from
  `X-Forwarded-For`, and only from a connection that comes from loopback.
- **The relay is another process** (scope ruling of the integrator): two processes cannot share
  the UDP port, so `sfcop-relay` owns it and the bindings. The lobby issues tickets signed with
  the shared key and, when a session leaves its table (closed or expired), makes one call to the
  relay: `DELETE /v1/bindings/<session>` on `127.0.0.1:47711` (`SFCOP_RELAY_HEALTH_PORT`),
  answered 204 whether or not the session had bindings (`RelayClient.h`). A relay outage never
  blocks removal: the failure is logged at debug and the relay's bindings lapse after 45 s anyway.
  Ticket rule of the relay, kept here: peer 1 is the host with role 1, peers 2 and up have role 2.
  The lobby's own loopback health listener is therefore on 47712, not 47711.
- **Logging** goes to stdout as `<UTC time> <level> <tag>: <sentence>`; systemd appends it to
  `/var/log/sfcop/lobby.log`. Nothing identifying is logged above debug: per request the method,
  the route pattern (session id left out), the status and the duration; per event a session's
  random id and player count. The one place an address appears is a rate-limit or block event, as
  LOBBY_API.md says; it is logged once a minute per address. Tests assert this.

## Layout

```
server/CMakeLists.txt        standalone project; add_subdirectory(server) also works
server/lobby/                the lobby: *.cpp, *.h, *Test.cpp, main.cpp, Server.cpp (sockets)
server/deploy/               deploy.sh, units, health check, logrotate, env example, container test
```

## Build and test

```
cmake -S server -B work/server -G Ninja && cmake --build work/server -j4 && ctest --test-dir work/server
python3 -m pytest tests/test_server.py       # the same, plus the deploy kit's script tests
```

`scripts/check.sh` runs `tests/test_server.py` with the rest of `tests/`, which builds and runs the
doctest runner (`sfcop_lobby_tests`, no sockets and no clock) into `$SFCOP_SERVER_BUILD` or
`<work>/server-tests`. Wiring the services into the root CMake build (so `ctest --preset` sees
them too) is a one-line `add_subdirectory(server)` in `CMakeLists.txt`, the integrator's.

## Configuration

Environment variables, from two files outside the repository: `/etc/sfcop/sfcop.env` (mode 600,
root; holds only `SFCOP_TICKET_KEY`, shared with the relay, written once by whichever deploy runs
first) and `/etc/sfcop/lobby.env` (mode 640, `root:sfcop`; `deploy/lobby.env.example` is the
template):

| Name | Meaning | Default |
| --- | --- | --- |
| `SFCOP_TICKET_KEY` | 64 hex digits, shared with the relay; required (in `sfcop.env`) | none |
| `SFCOP_HTTP_LISTEN` | The API, plain HTTP behind the TLS proxy | `127.0.0.1:8080` |
| `SFCOP_HEALTH_LISTEN` | The same API for the host's monitor | `127.0.0.1:47712` |
| `SFCOP_RENDEZVOUS_HOST` | The relay's name, which clients are told to bind at | `localhost` |
| `SFCOP_RELAY_PORT` | The relay's UDP port, as told to clients | `47710` |
| `SFCOP_RELAY_HEALTH_PORT` | The relay's loopback port for the removal call | `47711` |
| `SFCOP_BLOCKLIST` | Comma-separated IPv4 addresses answered `forbidden` or not at all | none |
| `SFCOP_ALERT_COMMAND` | Run once when a health check starts failing; may use `$SFCOP_ALERT_MESSAGE` | none |
| `SFCOP_LOG_LEVEL` | `debug` adds the chatty lines | `info` |

## Deploying

On a fresh Debian or Ubuntu host, as root, from a checkout:

```
sudo SFCOP_LOBBY_DOMAIN=lobby.example.test sh server/deploy/deploy.sh
```

Then edit `/etc/sfcop/lobby.env`: set `SFCOP_RENDEZVOUS_HOST` to the relay's public name. The
service refuses to start while it is unset or still the template's `lobby.example.test`, and
`deploy.sh` does not start it until it is edited (its last lines say what to run). Also set
`SFCOP_ALERT_COMMAND` to whatever sends the owner one message, then
`sudo systemctl enable --now sfcop-lobby.service sfcop-health@lobby.timer`. The script is safe to
run again: an existing environment file is never touched, and the ticket key in `sfcop.env` is
written only if absent (the relay's installer does the same, so the first to run wins). It
installs the build tools,
builds the service into `/opt/sfcop/bin`, creates the `sfcop` user, installs the units, a
`sfcop-health@lobby.timer` that runs `sfcop-health-check` every minute (one message when the
service goes from healthy to failing, none while it stays down), and a logrotate rule (daily,
7 files). It binds only where the environment file says and announces nothing.

**Container test.** `sh server/deploy/test-container.sh` runs `deploy.sh --no-systemd` on the
Debian 13 image that `cmake/lifter/Dockerfile` pins (by digest), then checks the installed files
and modes, the unit files with `systemd-analyze`, that a second run keeps the environment file,
a real register and list over HTTP, that the key file is root-only, that the log holds no address, name or secret, and the
monitor's one-message-per-failure behavior. It downloads packages, so `check.sh` does not run it;
the last lines of its run are in the pull request. It does not exercise the removal call against
a real relay (the relay is N4's) or the Caddy header path; the first is covered by the loopback
listener tests in `RelayClientTest.cpp`, the second not yet.

## Hosting, under 5 USD a month (DESIGN §11)

The service is a few MB of RAM and a few KB/s; any 512 MB, one-core host runs it. Prices move
and the sources disagree (checked 2026-11-07): the owner confirms on the provider's page.

| Option | Shape | Cost |
| --- | --- | --- |
| DigitalOcean Basic droplet, regular SSD | 1 vCPU, 512 MiB, 10 GiB, 500 GiB transfer | listed at 4 USD a month; one source says the plan was discontinued |
| Hetzner Cloud, smallest shared x86 plan | 2 vCPU, 4 GiB, 20 TB transfer | about 4 to 5.5 EUR a month depending on the source; raised in 2026, so it may be over 5 USD |

A domain is about 1 USD a month on top (DESIGN §11). Neither option is chosen here.

## The client's configuration (DESIGN §5.12; N6 implements the reading)

The port ships the lobby's address and the player may edit it. The client reads one JSON file,
`servers.json`, from the player's data folder (the Store component's per-user folder); when the
file is absent it uses the list compiled in.

```json
{
  "version": 1,
  "lobbies": [
    { "name": "Official", "url": "https://lobby.example.test" }
  ],
  "rendezvous": null
}
```

- `lobbies`: tried in order; the first that answers within the client timeouts of LOBBY_API.md
  serves the browser. `url` is the base without `/v1`, scheme `https`.
- `rendezvous`: normally `null`, because the lobby tells the client where to bind in every
  registration and ticket answer. A private setup may force `{ "host": "...", "port": 47710 }`.
- Unknown keys are ignored; a missing or malformed file falls back to the compiled-in list and is
  logged by name only.
