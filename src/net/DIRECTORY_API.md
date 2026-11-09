# Dynaverse stand-ins: directory, account login and version check

The original reaches the Dynaverse through online services that no longer exist: a directory that
lists the campaign servers and a firewall detector, an account login, a profile search, a version
check and a CD-key check. DESIGN §5.13 Phase A needs the client to log on through stand-ins for
them to a self-hosted campaign server. This document is the wire contract the stand-ins speak. It
is the original's protocol, recovered from the exe, so every byte layout carries the address it was
read at (`STARFLEETOP 0x...`, the retail 2.5.5.2 exe). Nothing here is ours to change: the client
is the original's code. The two services are `sfcop-directory` (`server/directory/`) and
`sfcop-auth` (`server/auth/`).

Words used below: *the client* is the game; *the stand-in* is our service; *inferred* means read
from the shape of the code but not traced to the end; *unknown* means not determined.

## Conventions

- **Integers** are little-endian unless marked BE.
- **`wstr`**: u16 character count, then that many UTF-16LE code units, no terminator. The count is
  read as signed (`STARFLEETOP 0x006E19E0`), so the stand-in keeps it below 0x8000.
- **`str8`**: u8 length, then the bytes; read as signed, so at most 127 (`STARFLEETOP 0x006E1B40`).
- **`blob16`**: u16 length, then the bytes; at most 32767 (`STARFLEETOP 0x006E1C60`).
- **`dstr`**: u32 length, then the bytes, no terminator (`STARFLEETOP 0x0042A397`).
- **Text messages** (account login, profile search) are `\key\value` pairs, ASCII, each message
  ending in `\final\`. A value cannot contain a backslash. The stand-in rejects a message longer
  than 4 KB.
- **Versions.** The original's protocols carry no version the stand-in can choose: the client
  speaks 2.5.5.2's and nothing else. The health endpoints carry `api: 1`. A change to this
  document is a new row in "Versions".
- **Invented values.** Every example uses invented keys, names and RFC 5737 addresses.

## Pointing the client at the stand-ins

The client carries the host names of the original services (each cited below by the address where
the exe holds it; the names themselves appear only in `src/app/ServiceHosts.cpp`) and resolves each
one through `gethostbyname`. The port's winsock module answers each of these names, without regard to case, as
the host the player's `servers.json` names for that service (`src/app/ServiceHosts.h`,
`src/hle/winsock/README.md`, "Aliases"; `server/README.md`, "The client's configuration"). Every
other name resolves as asked. Ports are the original's and cannot be configured on the client.

| Original host | Port | Read at | `servers.json` key |
| --- | --- | --- | --- |
| the directory hosts (three) | 15101/TCP | the server setup file's directory-address key, read by `STARFLEETOP 0x005D4EED`; tried in order | `directory` |
| the login host | 29900/TCP | `STARFLEETOP 0x006BAB30` | `auth` |
| the profile-search host | 29901/TCP | `STARFLEETOP 0x006BB930` | `auth` |
| the version-check host | 80/TCP | the URL at `STARFLEETOP 0x008884A0`, used by `0x006C99B0` | `patch` |
| the CD-key host | 29910/UDP | `STARFLEETOP 0x006C1960`, hosting side only (see "CD-key check") | `cdkey` |
| the original list host | 28900/TCP, 27900/UDP | `STARFLEETOP 0x006CB690` (the list, 28900/TCP) and `0x006CCB20` (hosting, 27900/UDP); the Campaign Selector's rows come from the list (`server/directory/README.md`, "Boundary for V2 and V1b"), which no stand-in serves yet | `directory` |

A service `servers.json` does not name resolves as `services.example.test`, a reserved name that
resolves nowhere: an unconfigured client fails at its first lookup and reaches no one else's
server. The firewall detector and the campaign servers are not named: the directory hands out
their addresses (below).

## The logon, in order

From the Dynaverse button to the Campaign Selector (`tWonLogon`, constructed at `STARFLEETOP 0x005D4EED`):

1. **Version check** (HTTP, `patch`), started at `STARFLEETOP 0x005D63E8`.
2. **Two directory queries** at once (`directory`): the firewall detectors (`0x005D5FBE`) and the
   campaign servers (`0x005D5D92`).
3. **Firewall detection** against the detector the first query returned (`0x005D6200`).
4. **The logon screen**: the player types an email, a nick and a password. The client asks the
   profile search whether the email is valid (`0x005DA82F`), then logs in to the account service
   (`0x005DAD90`). A first visit uses the screen's new-account button instead (`0x005DA980`),
   which creates the account and then logs in.
5. **The Campaign Selector** opens after the logon and queries the campaign servers again. Its
   rows do not come from that query: they come from the original list service and a status query
   to each server (`server/directory/README.md`, "Boundary for V2 and V1b"). The directory's
   records are read when the player joins a row, matched to it by name (`0x005D5E44`).

## Directory (TCP 15101)

### Framing

| Fact | Evidence |
| --- | --- |
| TCP; the client sends first; no handshake, no authentication, no encryption | `STARFLEETOP 0x006DEA00`, `0x006DEBC0`, `0x006DF6D0` |
| One request per connection; the client closes when its reply is complete. The two logon queries arrive as two concurrent connections | `0x006DF7D0`, `0x006E08F0` |
| Every frame starts with a u32 length that **counts itself**; the client requires `4 < length < 0x3E800` | `0x006E3570`, `0x006D0E80`, `0x006D1030` |
| Then a header: u8 5, u16 service, u16 message type. A reply with another header makes the client try the next address | `0x006E9330`, `0x006E8820` |
| Timeouts: 20 s each for connect, send and receive, inside 300 s for the whole query | `0x006DEA00`, `0x005D5E14` |

### GetDir request (service 2, message 0x0067)

| Offset | Width | Field | Value |
| ---: | ---: | --- | --- |
| 0 | 4 | length | the whole frame |
| 4 | 1 | header | 5 |
| 5 | 2 | service | 2 |
| 7 | 2 | message | 0x0067 |
| 9 | 4 | flags | 0x06020837 (firewall query), 0x06020E77 (campaign query) |
| 13 | 2 | unknown | 0 |
| 15 | wstr | path | `/TitanServers/Firewall` (`0x005D4DA7` from `0x00874020`), or the server setup file's directory-path key, retail `/StarFleetCommand2/Game/Release` |
| .. | 2 | data-type filter count | 0; then that many `str8` (none) |

Built at `STARFLEETOP 0x006E9330`. The campaign query, 81 bytes:

```
51 00 00 00 05 02 00 67 00 77 0e 02 06 00 00 1f 00 2f 00 53 00 74 00 61 00 72 00 46 00 6c 00
65 00 65 00 74 00 43 00 6f 00 6d 00 6d 00 61 00 6e 00 64 00 32 00 2f 00 47 00 61 00 6d 00 65
00 2f 00 52 00 65 00 6c 00 65 00 61 00 73 00 65 00 00 00
```

### GetDir reply (service 2, message 3)

| Offset | Width | Field | Value |
| ---: | ---: | --- | --- |
| 0 | 4 | length | the whole frame |
| 4 | 1 | header | 5 |
| 5 | 2 | service | 2 |
| 7 | 2 | message | 3 |
| 9 | 2 | status | i16; negative stops the query with that error (−1200: directory not found) |
| 11 | 1 | sequence | bit 7 set: last message; clear: another reply frame follows on the same connection |
| 12 | 4 | flags | **the reply's flags drive the parse**, not the request's; the stand-in echoes the request's |
| 16 | 2 | entity count | n |
| 18 | .. | entities | n of them |

Parsed at `STARFLEETOP 0x006E8820`. Each entity holds these fields, in this order, each only if
its flag is set:

| Field | Encoding | Flag |
| --- | --- | --- |
| type | u8 `'S'` (service) or `'D'` (directory) | 0x10 |
| service: path, name, address | wstr, wstr, u8 length + bytes | 0x01000000, 0x02000000, 0x04000000 |
| directory: path, name, a bool | wstr, wstr, u8 | 0x00010000, 0x00020000, 0x00040000 |
| display name | wstr | 0x20 |
| u32, unknown | u32 | 0x100 |
| date (a `time_t`, inferred) | u32 | 0x40 |
| u32, unknown | u32 | 0x80 |
| u32, unknown | u32 | 0x2000 |
| data objects | u16 count, then per object a `str8` type (0x200) and a `blob16` payload (0x400) | 0x800 |

The **address** is 6 bytes: the port, BE, then the IPv4 address in network order
(`STARFLEETOP 0x006E87B0`, `0x006CF5A0`). Both logon queries leave 0x01000000 clear, so an
`'S'` entity in either reply is: type, name, address, display name, then the fields of the other
set bits. The firewall reply (0x06020837) ends with a data-object count of 0; the campaign reply
(0x06020E77) adds the date and the data objects with their types and payloads.

### What the client takes from the replies

- **Firewall detectors** (`STARFLEETOP 0x005D617D`): every `'S'` entity whose name equals
  `TitanFirewallDetector` (`0x005D4E0D`, from `0x00874050`). Its address is where the client sends
  the detection request. No such entity: the logon fails with "You are behind a firewall".
- **Campaign servers** (`STARFLEETOP 0x005D6621`): every `'S'` entity with a data object of type
  `class tCampaignInfo`. The payload is a `tCampaignInfo` stream (below), the entity's address is
  the campaign server's, and its date orders the list (`0x005D6852`). The entity's name is not
  checked.

### `tCampaignInfo` (the payload, version 3)

Read with input flags 7 (`STARFLEETOP 0x006F9140`): a u8 version, then a u32 object id that must
equal the CRC of the class's decorated name, then the fields. The CRC is MSB-first CRC-32,
polynomial 0x04C11DB7, initial 0xFFFFFFFF, no reflection, no final XOR (`0x006FD970`,
`0x006FD9B0`). The field names are the original's, from the column list at `0x0085CC4C`, in
stream order (`0x00548E84`).

| # | Encoding | Field |
| ---: | --- | --- |
| 1 | u8 | version: 3 |
| 2 | u32 | object id 0xF5E54274 = CRC(`.?AVtCampaignInfo@@`) |
| 3 | u8, u32, u32 | `ID`: a nested `tID` (version, id 0x2B64DAB0 = CRC(`.?AVtID@nDatabase@@`), value) |
| 4 | u32 | `LockID` |
| 5 | 5 × dstr | `TemplateName`, `Description`, `EarlyMapName`, `MidMapName`, `LateMapName` |
| 6 | u32 n, n × dstr | `MissionList` |
| 7 | u32 n, n × u32 | `RaceList` |
| 8 | 3 × u32 | `CampaignEra`, `CampaignDifficulty`, `TriggerPrestige` |
| 9 | 3 × dstr | `TriggerMission`, `CampaignName`, `CharacterName` |
| 10 | u32 | `CharacterRace` |
| 11 | dstr | `ServerPassword` |
| 12 | u8, u32, 9 × u32 | `CurrentTime`: a nested `tCurrentTime` (version, id 0xB6620045 = CRC(`.?AVtCurrentTime@@`), nine u32) |
| 13 | 4 × u32 | `PlayersCurrent`, `PlayersMax`, `PlayersLoggedOnCurrent`, `PlayersLoggedOnMax` |
| 14 | 2 × dstr | `ServerVersion`, `ValidClientVersion`; the constructor's default for both is `2.5.5.2` (`0x005484C7`) |

`locked` has a column but no stream field. The Campaign Selector does not draw these records;
at join the client finds the record whose `TemplateName` is the selected row's name
(`0x005D5E44`, from `0x005D795F`), so a campaign's `TemplateName` must be the name its server
gives in its status reply. The stand-in sends the configured values.

### The stand-in

`sfcop-directory` answers every GetDir with status 0, the sequence byte 0x80 and the request's
flags. For `/TitanServers/Firewall` it returns one `'S'` entity named `TitanFirewallDetector`
with the detector's public address. For the configured campaign path it returns one `'S'` entity
per configured campaign server: name and display name the campaign's name, the address of the
campaign server, the date of the entry, and one data object `class tCampaignInfo` with the
payload above. Any other path: status −1200, no entities. A malformed frame closes the connection
without a reply. The campaign servers are a static file in V1 (`server/directory/README.md`); a
running campaign server registering itself is a later package.

## Firewall detection (TCP, the detector's address)

Started once per port in turn: 2300 to 2309, then 47624 (`STARFLEETOP 0x005D56BA`). Before each
request the client listens on that port (`0x006E4BC0`). The frame has a **2-byte** length that
counts itself (`0x006E48D0`).

Request (`STARFLEETOP 0x006E4D30`), 15 bytes:

```
0f 00  05  03 00  01 00  fc 08  01  20 4e 00 00  00
len    hdr svc    msg    port   ?   wait 20000   ?
```

Reply (`STARFLEETOP 0x006E4E70`), 9 bytes: `09 00 05 03 00 02 00 SS SS`, an i16 status. Status 0
(or 0x2740, which comes from the client's own bind) passes the step; anything else fails the logon
with "You are behind a firewall". The original detector presumably connected back to the port;
the client cannot tell, so **the stand-in answers 0 without connecting back**, and closes.

## Version check (HTTP, port 80)

Sent by `STARFLEETOP 0x006C99B0` on the client's own HTTP client:

```
GET /motd/vercheck.asp?productid=808&versionuniqueid=2.5.5.2&distid=270 HTTP/1.1
Host: <patch host>
```

`productid` and `distid` are the values at `0x008748C0` and `0x008748C8`; `versionuniqueid` is the
version string at `0x00874000`. The reply body is backslash pairs, read at `STARFLEETOP 0x006C9A40`:

| Key | Meaning to the client |
| --- | --- |
| `\newver\` | a non-zero number: a newer version exists; absent or 0: current |
| `\lockout\` | non-zero: the newer version is required |
| `\fpfileid\`, `\newvername\`, `\dlurl\` | the patch's id, name and download address |

The client marks itself out of date only when both `newver` and `lockout` are non-zero
(`0x005DA4A8`, `0x005D69A3`); then the logon stops at "Need_Patching". **The stand-in answers
every GET for that path** with:

```
HTTP/1.0 200 OK
Content-Type: text/plain
Content-Length: 9
Connection: close

\newver\0
```

and closes. Any other path is 404. A failed request is also read as current by the client
(`0x006C9C30` reports "no new version"), so the version check is the one stand-in whose absence
does not stop the logon.

## Account login (TCP 29900) and profile search (TCP 29901)

### Profile search: the email check

Before it logs in, the client asks whether the email is valid (`STARFLEETOP 0x006BBD10`,
`0x006BBDA0`) on its own connection:

```
client: \valid\\email\testcaptain@example.test\final\
stand-in: \vr\1\final\
```

`\vr\` 0 makes the client show "The email supplied is not valid." and stop (`0x005DAD9A`);
anything else passes. **The stand-in answers 1 when it holds an account for that email, and 0
otherwise.** The email is compared lower-cased (the client lower-cases it, `0x006BAD30`). Any
other request on this port closes the connection.

### Account login

The stand-in speaks first (`STARFLEETOP 0x006BB1D0`, state 1):

```
\lc\1\challenge\ABCDEFGHIJ\id\1\final\
```

The challenge is 10 letters `A` to `Z` from the stand-in's random source. The client then sends
one of two messages.

**A logon** (`STARFLEETOP 0x006BAF20`):

```
\login\\challenge\<client challenge>\user\<nick>@<email>\response\<R>\port\<p>\id\1\final\
```

with `\userid\` and `\profileid\` added when the client remembers them, and `\firewall\1` when it
is behind one. With `H = md5hex(password)` (32 lowercase hex digits, `0x006C14C0`), the format at
`0x0088781C` and 48 spaces at `0x0088782C`:

- `R = md5hex(H + 48 spaces + user + client challenge + server challenge + H)`
- the client expects `proof = md5hex(H + 48 spaces + user + server challenge + client challenge + H)`

where `user` is `<nick>@<email>` exactly as sent. The stand-in answers (state 2):

```
\lc\2\sesskey\<n>\proof\<proof>\userid\<u>\profileid\<p>\id\1\final\
```

The client requires `sesskey`, `userid`, `profileid` and `proof`, and compares all 32 digits of
the proof with its own; a mismatch is "Could not authenticate server" (error 0x108) and the logon
stops. **The stand-in does not check `R`**: it computes the proof from the H it holds for that
email. A wrong password therefore fails in the client's own check, not in the stand-in.

**A new account** (`STARFLEETOP 0x006BB140`, the new-account button):

```
\newuser\\email\<email>\nick\<nick>\password\<password>\id\1\final\
```

The stand-in answers (state 3) `\nur\\userid\<u>\profileid\<p>\id\1\final\`; the client then
sends the logon above on the same connection.

**Every reply on this connection carries `\id\1`.** The client hands a message to the operation
waiting for it by its `\id\` (`STARFLEETOP 0x006BA260`, the string at `0x008874FC`; the operation
is looked up at `0x006BFAC0`); a message without one is read only as an error, `\bm\` or `\ka\`,
and is otherwise dropped, which leaves the client waiting.

**Errors.** `\error\\err\<code>\fatal\\errmsg\<text>\id\1\final\` (`STARFLEETOP 0x006BA780`). The
client treats 0x106 and 0x201 specially and every other code as a failed logon; the stand-in uses
0x103 for "no such account". After the logon the stand-in keeps the connection open and ignores
further messages until the client closes it or 10 minutes pass with nothing received.

Worked example (every value invented; computed with Python's `hashlib`, outside the code):

| Value | |
| --- | --- |
| password | `throwaway` |
| H | `8957a7e865c41c02e58f18bcd7c9f2b9` |
| user | `TestCaptain@testcaptain@example.test` |
| server challenge | `ABCDEFGHIJ` |
| client challenge | `abcdefghijklmnopqrstuvwxyz012345` |
| R | `ff6ea2a727e6b6599d9593a189db652a` |
| proof | `3c3391c2ffbea131d0a5e220b76ab18f` |
| proof from the password `wrong` | `21012fb266f527997d41f7e21946d197` (the client rejects it) |

### The stand-in's accounts (interim, DESIGN §12.3)

**An interim answer under DESIGN §12.3, not a decision.** The owner has not decided what the
native client does about the original's accounts and key check. Until then:

- A new-account request is always accepted, for any email, nick and password. A second request
  for the same email replaces the first.
- The stand-in keeps, per lower-cased email, only H, the nick, and the ids it assigned. They live
  in process memory: nothing is written to disk, and a restart empties the table, as the lobby's
  sessions are emptied (`LOBBY_API.md`). After a restart every player presses the new-account
  button once more.
- A logon for an email the stand-in does not hold gets error 0x103; the profile search already
  answered `\vr\0`, so the player sees the original's "The email supplied is not valid." and
  uses the new-account button.
- The password is read from the new-account request only to compute H and is then discarded. H
  and the password are never logged, never echoed in an error, never sent back except as the
  proof. A debug line names the email at most.

## CD-key check (UDP 29910)

The client reaches the CD-key host (`STARFLEETOP 0x006C1960`) only from the code that runs when it hosts a game
(`STARFLEETOP 0x006C1960`); the Dynaverse logon does not use it. V1 does not serve it, and
`servers.json` names its host only so that the name resolves to no one else's server. When a
package serves it, it follows the interim §12.3 rule: any key in the game's format is accepted,
and nothing about the key is verified, stored or logged.

## Health

Each service answers `GET /v1/health` on a loopback port, for the host's monitor, as the lobby
does (`LOBBY_API.md`, "Health"):

| Service | Listener | Body |
| --- | --- | --- |
| `sfcop-directory` | `127.0.0.1:47713` | `{"status":"ok","api":1,"service":"directory","entries":N,"uptime_s":S}`: N the campaign entries served |
| `sfcop-auth` | `127.0.0.1:47714` | `{"status":"ok","api":1,"service":"auth","accounts":N,"uptime_s":S}`: N the accounts held |

Another path is 404, another method 405.

## Security

**Interim, not decided.** These are the owner's open decisions under DESIGN §12.3 and §9.

- The original protocol carries the **new-account password in clear** over TCP, and the logon's
  response is a plain MD5 construction. The stand-in cannot change what the client sends. A player
  must use a **throwaway password** that they use nowhere else, and the README says so.
- Nothing is encrypted: the directory, the version check and the account login are plain TCP and
  HTTP, as in the original. No TLS is possible without changing the client.
- Whether accounts persist, whether the account login is protected in transit, and what the key
  prompt and the key check do are the owner's to decide (DESIGN §12.3, §9). The stand-ins keep
  nothing on disk until then.
- The stand-ins verify nothing about a player. The account login's only check is the client's own
  check of the proof.

## What is logged

- **Per connection:** the service, the request type (`getdir`, `detect`, `vercheck`, `valid`,
  `login`, `newuser`), the result and the duration. No address above debug.
- **Never:** a password, H, a proof, a response, a challenge, a key, a session key. An email
  appears only at debug.
- Logs go to stdout, appended by systemd to `/var/log/sfcop/<service>.log`, rotated daily, kept
  7 days (DESIGN §10).

## Versions

| Version | Date | Package | Change |
| ---: | --- | --- | --- |
| 1 | 2026-11-08 | V1 | First version: directory, firewall detection, version check, profile search, account login, health |
