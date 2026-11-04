# src/hle/winsock: WSOCK32 on Transport

Winsock 1.1 as the game calls it: the 28 `WSOCK32.DLL` rows of `src/hle/surface.tsv`, on the
`Transport` component (`src/platform/Transport/Transport.h`). Every socket call the game makes goes
through this module to the host sockets backend (`TransportHost.cpp`).

| File | What |
| --- | --- |
| `Winsock.h`, `Winsock.cpp` | The module: `sfcop::hle::winsock::Winsock`, one method per entry, plus the stateless byte-order and address functions |
| `WinsockApi.h`, `WinsockApi.cpp` | C entry points over one process-wide instance (`guestWinsock()`, made on `makeTransport()`), with the host-side forms of `sockaddr_in`, `fd_set`, `timeval`, `WSADATA` and a host entry |
| `WinsockGuest.c` | The guest shims, exporting only `sfcop_hle_winsock[]`: each row by name and by ordinal (`wsock32.dll#N`). Built only with the lift |
| `WinsockTest.cpp` | Each entry as the game calls it, and its error paths, through `TransportMock`; the C entry points |
| `SelectTest.cpp` | `select`: the `fd_set` translation, the timeout conversion, a non-blocking connect's end; `__WSAFDIsSet` |
| `AddressTest.cpp` | `htons`, `ntohs`, `htonl`, `ntohl`, `inet_addr`, `inet_ntoa` with hand-written values |
| `LoopbackTest.cpp` | The module on the real host transport over loopback, including the Phase 1 spike's Winsock checks |

## How it behaves

- **Sockets.** A guest `SOCKET` is a number this module hands out (multiples of 4 from 0x100),
  mapped to a Transport `SocketId`. Only `AF_INET` stream (TCP) and datagram (UDP) sockets exist;
  anything else fails as Winsock would (`WSAEAFNOSUPPORT`, `WSAESOCKTNOSUPPORT`,
  `WSAEPROTONOSUPPORT`). Every call before `WSAStartup`, or after the last `WSACleanup`, fails with
  `WSANOTINITIALISED`; the last `WSACleanup` closes the sockets left open.
- **Errors.** `WSAGetLastError` is per thread. Transport errors map through one table
  (`wsaErrorOf`); a call that means something else by an error says so where it fails (a connect
  in progress is `WSAEWOULDBLOCK`, a second one `WSAEALREADY`, an oversized datagram
  `WSAEMSGSIZE`). A call blocked on a socket that another thread closes fails with `WSAEINTR`.
- **Blocking.** Sockets block until `ioctlsocket(FIONBIO)` makes them non-blocking. A shim gives
  the machine up around a call that can wait.
- **Non-blocking connect**, as the game uses it: `connect` returns `WSAEWOULDBLOCK`; `select` on
  `writefds` and `exceptfds` reports success as writable and failure in `exceptfds`; then
  `getsockopt(SO_ERROR)` reads 0 or `WSAECONNREFUSED`.
- **select.** `nfds` is ignored. A NULL timeout waits forever; a part of a millisecond waits a
  whole one, so a short timeout is not a poll. Every set keeps only its ready sockets, in order.
- **Options.** `setsockopt` and `getsockopt` take `SO_REUSEADDR`, `SO_KEEPALIVE`, `SO_BROADCAST`
  (UDP), `SO_LINGER`, `SO_DONTLINGER` (TCP), `SO_SNDBUF`, `SO_RCVBUF`, `TCP_NODELAY`; `getsockopt`
  also `SO_TYPE` and `SO_ERROR`. Any other option is `WSAENOPROTOOPT`. `ioctlsocket` takes
  `FIONBIO`, `FIONREAD` and `SIOCATMARK`.
- **Names.** `gethostbyname` resolves through `Transport::Resolve` (a dotted quad, a name, or this
  machine's own name, which is its interfaces' addresses); NULL or an empty name is this machine.
  The shim keeps one `hostent` and one `inet_ntoa` buffer per thread in guest memory, as Winsock
  does. Up to 16 addresses are returned.

## Not provided

- `recv` and `recvfrom` flags (`MSG_PEEK`, `MSG_OOB`) and `send` with `MSG_OOB`: `WSAEOPNOTSUPP`.
  The game passes 0.
- A datagram longer than the buffer is truncated without `WSAEMSGSIZE`: the Transport does not say
  that it truncated.
- `WSAAsyncSelect`, `WSACancelBlockingCall` and the other Winsock calls the game does not import.
- Rendezvous (Phase 3): `Transport::GetRendezvous` returns `kUnsupported`.
- Windows: `makeTransport()` returns the stub there until a Winsock2 backend is written, so every
  socket call fails with `WSAEOPNOTSUPP`.

## Tests

```bash
cmake -S . -B <build> -G Ninja -DSFCOP_LIFT=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build <build> -j4
<build>/sfcop_tests --source-file='*hle/winsock/*'
```

`LoopbackTest.cpp` is 5 test cases: the spike's 13 Winsock checks in the first (each named as the
spike named it), then UDP, a non-blocking connect, a close that wakes a blocked `recvfrom`, and a
timed `select`. The two timed tests (here and in `TransportHostTest.cpp`) measure a 200 ms
`select` with `std::chrono::steady_clock` and accept 195 to 400 ms; `SFCOP_SKIP_TIMING_TESTS=1`
skips them.
