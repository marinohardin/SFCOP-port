# server/auth: the account login and version check stand-in

`sfcop-auth` stands in for the original account login, its profile search and the version check,
so that the original client, pointed here by `servers.json` (`server/README.md`, "The client's
configuration"), passes the version check and logs on to the Dynaverse (DESIGN §5.13 Phase A).
The wire contract is `src/net/DIRECTORY_API.md`, "Version check" and "Account login"; every
message there carries the exe address it was read at.

## Interim, under DESIGN §12.3: not a decision

The owner has not decided what the native client does about the original's accounts and key
check (DESIGN §12.3, §9). Until then this service:

- **accepts every new-account request**, for any email, nick and password; a second request for
  the same email replaces the first;
- **keeps, per email, the MD5 hash H of the password**, the nick and two ids it assigned, in
  process memory, because the logon's proof is computed from H. H is password-equivalent for that
  proof: it is held in memory only and **a restart empties the table**, so every player presses
  the logon screen's new-account button once more. It is never written to disk, logged or
  echoed. The password itself is read only to compute H; the parsed request and the receive
  buffer that held it are overwritten once the request is handled;
- **verifies nothing.** It answers a logon with the proof the client checks, computed from the H
  it holds; a wrong password fails in the client's own check of that proof. An email it does not
  hold is told "not valid" by the profile search and refused by the login;
- **never writes, logs or echoes** a password, H, a proof, a challenge or a session key. A debug
  line names the email at most, and a client-supplied command word in a log line is cut at 32
  printable bytes. It has no key check at all: the client reaches the CD-key service
  only when it hosts a game, and V1 does not serve it (`DIRECTORY_API.md`, "CD-key check").

## Security: use a throwaway password

**The original protocol carries the new-account password in clear over TCP**, and the logon's
response is a plain MD5 construction anyone on the path can attack. The service cannot change
what the client sends, and there is no TLS. **Players must use a password they use nowhere
else.** Persistence, transport protection and the key check are the owner's open decisions
(`DIRECTORY_API.md`, "Security").

## What it does

- **TCP 29900, the account login.** Speaks first with a challenge; answers a new-account request
  and then a logon; stays open, ignoring later messages, until the client logs out, closes, or is
  quiet for 10 minutes.
- **TCP 29901, the profile search.** Answers the email check (`\vr\1` for an email it holds,
  `\vr\0` otherwise) and closes. Any other search request is closed unanswered.
- **TCP 80, the version check.** `GET /motd/vercheck.asp` answers `\newver\0` ("current"); any
  other path is 404. `off` serves nothing: the client then reads the check as current anyway, so
  on a host where the lobby's TLS proxy holds port 80, `deploy.sh` turns it off.
- **Loopback 47714, health.** `GET /v1/health` answers
  `{"status":"ok","api":1,"service":"auth","accounts":N,"uptime_s":S}`.

## Configuration

`/etc/sfcop/auth.env` (mode 640, `root:sfcop`; `deploy/auth.env.example`). Each variable has a
command-line option of the same meaning (`sfcop-auth --help`).

| Variable | Option | Meaning | Default |
| --- | --- | --- | --- |
| `SFCOP_LOGIN_LISTEN` | `--login-listen` | The account login | `0.0.0.0:29900` |
| `SFCOP_SEARCH_LISTEN` | `--search-listen` | The profile search | `0.0.0.0:29901` |
| `SFCOP_PATCH_LISTEN` | `--patch-listen` | The version check, or `off` | `0.0.0.0:80` |
| `SFCOP_HEALTH_LISTEN` | `--health-listen` | The health endpoint, loopback | `127.0.0.1:47714` |
| `SFCOP_LOG_LEVEL` | `--log-level` | `debug` adds the email of each request | `info` |

The unit grants `CAP_NET_BIND_SERVICE` and no other capability, for port 80. If port 80 cannot be
had, the service logs it and runs without the version check.

## Logging

One line per request: its kind (`newuser`, `login`, `valid`, `vercheck`) and its result. No
address and no email above debug; never a password, H, proof, challenge or session key at any
level (tested in `AuthTest.cpp` and by the container test).

## Cost

One process, about 2 MB plus roughly 200 bytes per account held (the unit caps it at 64 MB), no
disk beyond its log. Ports: 29900/TCP, 29901/TCP and 80/TCP public, 47714/TCP loopback. No
scheduled job besides the shared minute-by-minute health check, and no call to anything outside
the host.

## Files

| File | What |
| --- | --- |
| `Text.h` | The `\key\value...\final\` messages |
| `Md5.h` | MD5, which the original login is built on, tested on RFC 1321's vectors |
| `Accounts.h` | The memory-only account table and the proof |
| `Sessions.h` | `LoginSession`, `SearchSession`, `PatchSession`, on bytes |
| `Config.h` | The environment and the options |
| `TcpServer.h`, `Health.h`, `Log.h` | The poll() loop, the health endpoint and the log, as in `server/directory/` |
| `main.cpp` | `sfcop-auth` |
| `*Test.cpp` | doctests on hand-written bytes: the worked proof, a wrong password, an unknown email, a restart, the email check, the version check, the logs; no socket |

```
cmake -S server -B work/server -G Ninja && cmake --build work/server -j4
work/server/auth/sfcop_auth_tests
```
