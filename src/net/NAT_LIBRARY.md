# NAT traversal: libjuice, GameNetworkingSockets, or our own punch

A memo for DESIGN §12.8, which the integrator decides. It compares the two libraries the design
names with the third option, a hole punch written here over the Transport component. It ends with
a recommendation, and with the `third_party/INVENTORY.md` row a library would take if one is
chosen.

The facts about the libraries were read on 2026-11-06 from their repositories: README, LICENSE,
CMake files, public headers, release pages and commit history. The latest releases then were
libjuice 1.7.4 (2026-09-26) and GameNetworkingSockets 1.6.1 (2026-10-03). "Not verified" marks a
point that was not checked by building. No library was built for this memo.

## What the project needs

- **Two peers or a small mesh** (`PROTOCOL.md`, session messages), introduced by our own lobby and
  rendezvous. Both ends are ours. Nothing has to interoperate with anyone else's STUN, TURN or ICE.
- **The game's own reliability stays.** The session layer has its own sequenced channel, and the
  envelope has its own optional one (`PROTOCOL.md`). The traversal has to find a UDP path, keep it
  open, and hand it over.
- **It fits the Transport contract** (`src/platform/Transport/Transport.h`).
  `Rendezvous::ConnectToPeer` returns a Transport `SocketId`, and the dplay module sends and
  receives on it through the same interface it uses on the LAN. Tests run on `TransportMock` with
  no network and no real clock (`docs/CONVENTIONS.md` §6).
- **Three presets:** macOS arm64; Windows with clang-cl and lld-link against the MSVC CRT; Linux in
  the container.
- **Licence:** zlib, MIT, BSD or public domain until DESIGN §12.1 is decided
  (`docs/CONVENTIONS.md` §5).
- **A relay that forwards envelopes unchanged** (DESIGN §6.7), and a server side that fits one
  small VPS under 5 USD a month (DESIGN §11).

## Comparison

| | libjuice 1.7.4 | GameNetworkingSockets 1.6.1 | Own punch over Transport |
| --- | --- | --- | --- |
| What it is | An ICE agent in C (RFC 8445), with STUN, a TURN client and a STUN/TURN server | A whole connection-oriented messaging library in C++17: handshake, encryption, reliable and unreliable lanes, fragmentation, congestion control, and P2P through ICE | `RENDEZVOUS.md`: bind, exchange candidates, probe, fall back to our relay |
| Licence | **MPL-2.0**, file-level copyleft: outside the permitted set | BSD-3-Clause. Its dependencies: protobuf (BSD-3-Clause; version 22 and later pulls in abseil, Apache-2.0) and one crypto backend: OpenSSL 3 (Apache-2.0), libsodium (ISC) or Windows BCrypt. Avoiding Apache-2.0 needs an older protobuf plus libsodium or BCrypt | This project's own |
| Size | About 12,700 lines of C, server and TURN included; no required dependency (Windows links `ws2_32` and `bcrypt`) | About 65,000 lines of C/C++ (its native ICE client about 4,300), plus protobuf and a crypto library | About 600 to 1,000 lines of C++ in `src/net/` (estimate), plus the service in `server/` |
| Build on the three presets | CMake, C11, static target. Documented on Linux, macOS and Windows (MinGW, NMake). clang-cl: not verified, though it is plain C | CMake 3.15+, C++17, protoc and libprotobuf of matching versions, a crypto backend; the project's CI covers Linux, Windows through vcpkg and macOS. clang-cl: not verified. The build guide notes a known LLVM link issue. protobuf must be cross-built for the Windows preset | Nothing new: it builds with `src/` |
| Threads | Its own: one shared thread (poll mode), one per agent, or a socket shared by agents (mux mode) | Its own service thread | None: it runs on the calls and threads the dplay module already has |
| Sockets | Opens and owns its UDP sockets; no way to give it ours. Application data shares the socket with ICE | Opens and owns its UDP sockets; no hook to give it ours | Transport sockets, so `TransportMock` tests it |
| Fits the Transport contract | No. `ConnectToPeer` would have to return a socket the library owns. That needs a bridge socket pair, or a Transport backend wrapping the library, and neither can be tested on `TransportMock` | No, and worse: its unit is a connection with its own messages, not a datagram socket. The dplay module would have to be rewritten onto its message API, against DESIGN §4.3's "only the transport under them changes" | Its shape, yes: Transport sockets and `TransportMock` tests. Two additions are needed, as for any option (see "Recommendation") |
| What it adds on the wire | Nothing; datagrams pass through as they are | Its own handshake, AES-GCM encryption, acks and fragmentation, under ours: two reliability layers | Nothing beyond `PROTOCOL.md` |
| Relay | A TURN client, and a TURN server in the library. Our relay would have to be TURN, not "forwards envelopes unchanged" | A TURN client in its native ICE; no server. Its hosted relay service is proprietary and not part of the open-source build | Our relay, as `RENDEZVOUS.md` specifies; the same port as the rendezvous |
| Server side | Signalling (ours: the lobby) and a STUN/TURN server, which could be the library's own | Signalling (ours), plus a separate STUN and a TURN server | One UDP process: rendezvous and relay; one HTTPS process: the lobby |
| IPv6 | Yes | Yes | No in version 1; the Transport contract is IPv4 |
| Maintenance | Active: four releases in 2026, last commit 2026-11-05; essentially one maintainer | Active: three releases in 2026, last commit 2026-10-02; essentially one lead developer at one company | Ours |
| NAT coverage | Full ICE: every candidate pair, consent freshness, TCP candidates, TURN | Full ICE with TURN | Host and server-reflexive candidates only. A symmetric NAT on either side goes to the relay; no port prediction |

## Assessment

**GameNetworkingSockets is the wrong shape.** It replaces the transport and also the layer above
it. Under it, the dplay module's channel and the envelope's would run on top of its own
reliability and encryption. Using its P2P means protobuf and a crypto library on three presets,
plus a TURN server on the VPS. Its permissive licence holds only with care over its dependencies'
versions.

**libjuice is the right shape for ICE, but ICE is more than this project needs, and its licence is
not yet allowed.** It passes datagrams through untouched, it is small, and it ships its own server.
Against it:

- **Licence.** MPL-2.0 is outside the permitted set until the owner decides DESIGN §12.1. MPL's
  copyleft is per file, so using it unmodified is easy to comply with, but it is still the owner's
  call.
- **Contract fit.** It owns its sockets and threads, so it does not fit the Transport contract
  without a bridge. That bridge also takes the traversal out of reach of the deterministic tests.
- **Relay.** Its relay is TURN, so our relay would have to speak TURN, which DESIGN §6.7 does not
  describe. The alternative is to run its relay unused and write ours anyway.

**The project's own punch is enough.** The only work NAT traversal has to do here is to exchange
two or three candidates through a server we run, probe them, and fall back to a relay we run. All
of it is in `RENDEZVOUS.md`. It needs no dependency, no thread and no second server process. It
fits the contract's shape, sockets it does not own and calls it does not block, and it is tested on `TransportMock` like the rest of `src/`. What it
gives up is full ICE's coverage of unusual networks: no TURN-over-TCP, no IPv6, no port
prediction for symmetric NATs. Pairs that would need those take the relay, which DESIGN §5.11
already requires to work within 5 s.

## Recommendation

**Write the punch over Transport, as `RENDEZVOUS.md` specifies, and add no NAT traversal library.**
The deciding reasons:

1. It is the only option that keeps the Transport contract's shape and its mock-based tests; it
   needs only the two additions below, which every option needs.
2. Both ends are ours, so ICE's interoperability buys nothing, and the relay stays "forwards
   envelopes unchanged" (DESIGN §6.7).
3. It brings no licence question (libjuice is MPL-2.0; GameNetworkingSockets pulls Apache-2.0
   dependencies unless pinned with care).
4. There is no extra thread, socket owner or reliability layer under the game's own.
5. The server side is one UDP process and one HTTPS process, well inside the 5 USD ceiling.

**This is a contract change too.** `Transport.h`'s Rendezvous has only the connecting side,
`ConnectToPeer`. Two additions to `src/platform/Transport/Transport.h` are needed before N2 can
build it, whichever option is chosen:

- **An accepting side.** The peer being reached (the host, and every mesh peer) learns of the
  caller only from `PeerCandidates`, so it needs a call that returns that path's socket, for
  example `Result<SocketId> AcceptPeer(std::uint32_t timeoutMs)`, or a callback.
- **The lobby's credentials.** The session id and the ticket the lobby issued must reach the
  Rendezvous before it binds, for example a `Bind(sessionId, peerId, ticket)` call.

The integrator made that change in #60 (2026-11-07), which also decided DESIGN §12.8 for this
recommendation.

This departed from two documents, since changed by #60. DESIGN §12.8 frames the choice as "libjuice or
GameNetworkingSockets", and CONVENTIONS §5 lists "one NAT traversal library chosen in Phase 3" in
the dependency set and names ICE as a capability the project cannot reasonably write. The
recommendation is that the project does not need ICE, only the subset above. The sentences to
change are in the pull request description.

If the integrator chooses a library instead, it should be libjuice. That needs the owner to accept
MPL-2.0 first (DESIGN §12.1), and either a Transport backend that wraps it or a change to the
Rendezvous contract. The row would be:

| Name | Version | Pin | Licence | Where used | Why |
| --- | --- | --- | --- | --- | --- |
| libjuice | 1.7.4 (2026-09-26) | tag archive `libjuice-1.7.4.tar.gz` (`https://github.com/paullouisageneau/libjuice/archive/refs/tags/v1.7.4.tar.gz`), sha256 `95c08886...515ef798`; tag commit `b89c792e` | MPL-2.0 | `src/net/` (N2): the Rendezvous behind `Transport::GetRendezvous()` | ICE candidate gathering and connectivity checks, with TURN for the relay |

The full SHA-256 of that archive, computed on 2026-11-06, is
`95c088862aa1b88b73d62aa99577135010eb768fcafb252d13aa85a8515ef798`.
