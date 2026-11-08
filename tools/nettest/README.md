# nettest: the Rendezvous test harness

Tests that two native clients reach each other through `Transport::GetRendezvous()`
(`src/platform/Transport/Transport.h`, `src/net/RENDEZVOUS.md`): directly through a punched hole
when the network allows it, and through the relay within 5 s when it does not (DESIGN §5.11).

| File | What it is |
| --- | --- |
| `StandInService.h` | A stand-in for the rendezvous and relay service, and the lobby's ticket issuing. Test code only; N3 and N4 build the real services |
| `NatModel.h` | One simulated NAT: cone or symmetric mapping, address-and-port dependent filtering, and a switch that drops the punch (every `Probe` and `ProbeReply`) |
| `SimNetwork.h` | An in-memory network of hosts behind such NATs, for the in-process tests (`src/net/RendezvousEngineTest.cpp`) |
| `NetPair.h` | The two-process test: the parent runs the stand-in service and a UDP forwarder per client that does what `NatModel` says; two forked clients connect through it |
| `main.cpp`, `CMakeLists.txt` | `sfcop_nettest`, the harness as a program, for the runs below |

The same two-process scenarios are in `sfcop_tests` as `src/net/NetPairTest.cpp`. They use real
sockets, real processes and the real clock, so they run only when asked for:

```bash
SFCOP_NETTEST=1 ./sfcop_tests -tc="net pair*"
```

`sh scripts/check.sh` and `ctest` skip them. The unit coverage of the same paths, on simulated
time, is `src/net/RendezvousEngineTest.cpp`.

## Build

A CMake project of its own; it compiles the sources of `src/` it needs.

```bash
cmake -S tools/nettest -B ../work/nettest -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

```bash
cmake --build ../work/nettest -j4
```

macOS or Linux, with a C++17 compiler, CMake 3.28 and Ninja. On the owner's machine the build
directory goes under `work/` on the external drive.

## On one machine

```bash
../work/nettest/sfcop_nettest pair all
```

Four scenarios, each with a host process and a joiner process behind NATs of their own on
loopback. Each prints the path `ConnectToPeer()` took and how long it took:

| Scenario | The NATs | Expected |
| --- | --- | --- |
| `cone` | Both cone | `direct`, under 3000 ms |
| `symmetric` | The joiner's is symmetric | `relay`, 3000 to 5000 ms |
| `blocked` | Both cone; the host's drops the punch | `relay`, 3000 to 5000 ms |
| `forced` | Both cone; the joiner's test switch forces the relay | `relay`, under 1500 ms |

The exit status is 0 when all of them took the expected path in time. The clients offer no
local candidates here: on one machine they would reach each other around the NATs.

## On a real network (the owner's runbook)

This needs three machines:

- **S**, the service host: any machine with a public IPv4 address (or a port forward), with UDP
  port 47710 open to the Internet. It runs the stand-in service until N3 and N4 provide the real
  one. The owner names this host.
- **A** and **B**, the two clients, on two different networks behind their own routers (for
  example a home network and a phone's hotspot). For the symmetric-NAT test, one of them is on a
  network known to be symmetric (many mobile carriers are).

1. Build `sfcop_nettest` on all three machines (Build, above).
2. On any machine, make a test key and copy it to the three machines. It is a test value only,
   never the lobby's real key:

   ```bash
   openssl rand -hex 32
   ```

3. On S:

   ```bash
   ./sfcop_nettest serve --port 47710 --key <key>
   ```

   It prints the number of bindings, introductions and relayed envelopes every 10 s, and no
   addresses.
4. On A:

   ```bash
   ./sfcop_nettest host --service <S's host name or address>:47710 --key <key>
   ```

5. On B, within 2 minutes:

   ```bash
   ./sfcop_nettest join --service <S's host name or address>:47710 --key <key>
   ```

   B prints `join: direct <ms>` or `join: relay <ms>`, and A prints that the joiner said hello.
   Record the line. Two cone NATs give `direct`; a symmetric NAT on either side gives `relay`,
   under 5000 ms.
6. The forced fallback, DESIGN §5.11's test: run step 4 again on A, then on B:

   ```bash
   ./sfcop_nettest join --service <S's host name or address>:47710 --key <key> --force-relay
   ```

   Expected: `join: relay <ms>` with `<ms>` under 5000.
7. Stop S with Ctrl-C.

A `join: fail ...` line names the call that failed: `bind` (S not reachable, or UDP 47710
blocked), `connect HostUnreachable` (A not bound, or neither path worked), `exchange` (the path
opened and then lost the messages).

Status: **open**. No service host exists yet; it waits for the host the owner names (N3 and N4's
server, when there is one).

## Configuring the native client

The client finds the service through the environment until the lobby's address configuration
exists: `SFCOP_RENDEZVOUS=<host>[:<port>]`, and `SFCOP_RENDEZVOUS_FORCE_RELAY=1` for the test
switch. Without `SFCOP_RENDEZVOUS`, `GetRendezvous()` answers `kUnsupported`.
