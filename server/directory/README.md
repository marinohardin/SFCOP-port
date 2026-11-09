# server/directory: the directory stand-in

`sfcop-directory` stands in for the original directory service and its firewall detector, so
that the original client, pointed here by `servers.json` (`server/README.md`, "The client's
configuration"), finds a firewall detector and the campaign servers on its way to the Dynaverse
logon (DESIGN §5.13 Phase A). The wire contract is `src/net/DIRECTORY_API.md`, "Directory" and
"Firewall detection"; every byte layout there carries the exe address it was read at.

## What it does

- **TCP 15101, the directory.** One query per connection, answered and closed. The firewall
  query (`/TitanServers/Firewall`) gets one detector entity at `SFCOP_PUBLIC_ADDRESS` and the
  detector port; the campaign query (`SFCOP_CAMPAIGN_PATH`) gets one entity per server in the
  campaign file, each with its `tCampaignInfo` record; any other path gets "directory not found".
- **TCP 15102, the firewall detector.** Answers every well-formed detection request with
  success at once and closes. It never connects back to the client: the client cannot tell.
- **Loopback 47713, health.** `GET /v1/health` answers
  `{"status":"ok","api":1,"service":"directory","entries":N,"uptime_s":S}`.

A malformed frame, a frame of 0x3E800 bytes or more, or a second query on one connection closes
the connection without a reply. A connection that sends nothing for 60 s is closed. At most 256
connections are open at once.

## Keeps nothing

The campaign list is read once at start from the campaign file; the service holds no state of
its own and writes nothing but its log. A restart changes nothing a client can see.

## The campaign file

`/etc/sfcop/campaigns.txt` (`SFCOP_CAMPAIGN_FILE`; `deploy.sh` writes it once from
`deploy/campaigns.example`). One `[campaign]` block per server:

```
[campaign]
address = 192.0.2.10:27000      # required: where clients reach the campaign server
date = 0                        # the entry's date; the client sorts the list by it
CampaignName = TestCampaign
MissionList = first, second     # lists are comma-separated
RaceList = 1, 2
CurrentTime = 0,0,0,0,0,0,0,0,0 # nine numbers
```

Every key besides `address` and `date` is a field of the original's campaign record, by its
original name (`DIRECTORY_API.md`, "tCampaignInfo"); a field left out keeps its default, and both
version strings default to `2.5.5.2`. A key the file does not know, a bad number or a block
without an address fails the whole file at start, so a typo is never served as a default. In V1
the list is static: a running campaign server registering itself is a later package.

## Configuration

`/etc/sfcop/directory.env` (mode 640, `root:sfcop`; `deploy/directory.env.example`). Each variable
has a command-line option of the same meaning (`sfcop-directory --help`).

| Variable | Option | Meaning | Default |
| --- | --- | --- | --- |
| `SFCOP_PUBLIC_ADDRESS` | `--public-address` | This host's IPv4 address as clients reach it; required | none |
| `SFCOP_DIRECTORY_LISTEN` | `--listen` | The directory | `0.0.0.0:15101` |
| `SFCOP_DETECTOR_LISTEN` | `--detector-listen` | The firewall detector; its port is the one handed out | `0.0.0.0:15102` |
| `SFCOP_CAMPAIGN_FILE` | `--campaigns` | The campaign file | `/etc/sfcop/campaigns.txt` |
| `SFCOP_CAMPAIGN_PATH` | `--campaign-path` | The path the client asks under (its server setup file's `ServerPath`) | `/StarFleetCommand2/Game/Release` |
| `SFCOP_HEALTH_LISTEN` | `--health-listen` | The health endpoint, loopback | `127.0.0.1:47713` |
| `SFCOP_LOG_LEVEL` | `--log-level` | `debug`, `info`, `warn` or `error` | `info` |

## Logging

One line per query: the kind, the status, the number of entries. No address, at any level.

## Boundary for V2 and V1b: the Campaign Selector's rows

With this service and `sfcop-auth` the client gets through the Dynaverse logon to the Campaign
Selector: the directory, the firewall detector, the version check, a new account and the logon
are all answered here. The selector's list then stays empty, and no field of the directory's
`tCampaignInfo` can change that. Its rows come from elsewhere:

- **Where the rows come from.** The selector's refresh (`STARFLEETOP 0x0045EFE3`) takes its rows
  from the server-list browser (`0x005DB1D5`, built at `0x005DAF59`), not from the directory.
  The browser asks the original list service (the host name at `0x008885C0`, TCP port 28900;
  the connect is `0x006CB690`) for the servers' addresses, then sends a UDP status query to each.
  A row is added only for a status reply that ends `\final\` and comes from the queried address
  and port.
- **The one filter.** `0x0046064B` drops a reply whose `validclientver` is not exactly the
  string at `0x00842C2C`, `2.5.5.2`.
- **What the directory's record is for.** The `tCampaignInfo` records of the campaign query are
  read when the player joins (`0x005D5E44`, called from `0x005D795F`): the selected row is
  matched to a record by name, the reply's `hostname` against the record's first string
  (`TemplateName`). A row with no record of that name cannot be joined.
- **Still to be read.** The list request the client sends and the reply it expects
  (`0x006CB690` and what follows), and the status query's exact text.

So a visible row needs two more pieces. The first is a stand-in for the original list service
(V1b): the client points the service's host name at the directory's host already
(`src/app/ServiceHosts.cpp`). The second is a status reply from the campaign server, which is the
server kit's part in Phase A (V2). `src/net/DIRECTORY_API.md` keeps the wire details.

## Cost

One process, about 2 MB of memory (the unit caps it at 64 MB), no disk beyond its log, a few
hundred bytes per query. Ports: 15101/TCP and 15102/TCP public, 47713/TCP loopback. No scheduled
job besides the shared minute-by-minute health check, and no call to anything outside the host.

## Files

| File | What |
| --- | --- |
| `Wire.h` | The directory and detection frames: encoders, bounds-checked decoders, the object-stream CRC, `tCampaignInfo` |
| `Directory.h` | The two sessions, `GetDirSession` and `DetectSession`, on bytes |
| `Campaigns.h` | The campaign file |
| `Config.h` | The environment and the options |
| `TcpServer.h` | The poll() loop that carries bytes between sockets and sessions |
| `Health.h` | The loopback health endpoint |
| `main.cpp` | `sfcop-directory` |
| `*Test.cpp` | doctests: every message both ways on hand-written bytes, the sessions, the campaign file, the options, health; no socket |

```
cmake -S server -B work/server -G Ninja && cmake --build work/server -j4
work/server/directory/sfcop_directory_tests
```
