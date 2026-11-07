// Transport: IPv4 UDP and TCP sockets as the Winsock HLE module needs them, plus the Rendezvous
// sub-interface that Phase 3 fills in (DESIGN §4.7, §6.2, §6.7). The game's own Winsock calls
// (socket, bind, connect, listen, accept, send, recv, sendto, recvfrom, select, getpeername,
// getsockname, gethostbyname, ...) map one to one; the session layer of DESIGN §5.11 is built on
// these sockets in src/net/, not here.
//
// Sockets start blocking. A blocking call waits as the operating system does; a non-blocking one
// returns kWouldBlock (or kInProgress for connect) instead.
//
// Threading: every method may be called from any thread. Two threads may use one socket at once
// only as one sender and one receiver, as Winsock allows; Close() while another thread is blocked
// on the socket makes that call return kNotConnected.
#pragma once

#include "platform/Core/Handle.h"
#include "platform/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfcop::platform {

struct SocketTag {};
using SocketId = Handle<SocketTag>;

enum class SocketKind : std::uint8_t { kUdp, kTcp };

// An IPv4 address and port. `address` is in host byte order: 192.0.2.1 is 0xC0000201.
struct Endpoint {
    std::uint32_t address = 0; // 0 = any (bind); 0xFFFFFFFF = broadcast (UDP send)
    std::uint16_t port = 0;    // 0 = any free port (bind)

    friend bool operator==(const Endpoint &a, const Endpoint &b) {
        return a.address == b.address && a.port == b.port;
    }
    friend bool operator!=(const Endpoint &a, const Endpoint &b) { return !(a == b); }
};

enum class SocketOption : std::uint8_t {
    kNonBlocking,   // 0 or 1 (ioctlsocket FIONBIO)
    kBroadcast,     // 0 or 1; UDP may send to 0xFFFFFFFF
    kReuseAddress,  // 0 or 1
    kNoDelay,       // 0 or 1; TCP: send small segments at once
    kKeepAlive,     // 0 or 1; TCP
    kReceiveBuffer, // bytes
    kSendBuffer,    // bytes
    kLingerSeconds, // TCP; -1 = off, 0 = reset on close, n = wait up to n seconds
    kBytesReadable, // read only: bytes that a Receive() would return now (ioctlsocket FIONREAD)
};

enum class ShutdownHow : std::uint8_t { kReceive, kSend, kBoth };

struct Accepted {
    SocketId socket;
    Endpoint peer;
};

struct Received {
    std::size_t bytes = 0;
    Endpoint from;
};

// The sockets a Select() call watches; on success each list keeps only the ready ones, in their
// original order.
struct SelectSet {
    std::vector<SocketId> readable;  // data, an incoming connection, or end of stream
    std::vector<SocketId> writable;  // room to send, or a non-blocking connect finished
    std::vector<SocketId> errored;   // a non-blocking connect failed, or out-of-band data
};

// Phase 3: reaching the other machines of a session by the names the lobby gives (DESIGN §5.11,
// §5.12, §6.7, §12.8). The traversal is the project's own punch over this component's UDP
// sockets, with the project's relay as the fallback; the wire is `src/net/RENDEZVOUS.md`. A
// machine Bind()s once with the ticket the lobby issued, then ConnectToPeer() reaches a peer and
// AcceptPeer() answers one reaching it. Each successful call hands out a UDP socket of this
// Transport, owned by the caller, that talks to that one peer (directly or through the relay);
// Send() and Receive() work on it, Close() ends it.
//
// Names: `sessionId` is the lobby's session id, 16 lowercase hexadecimal digits; a peer is named
// `<session>:<peer>` with the peer id in 8 lowercase hexadecimal digits, and `<session>` alone
// names the host (peer 1). Any other form is kInvalidArgument. The ticket is opaque, 36 bytes.
//
// Service answers (`RvError` in RENDEZVOUS.md) map to: kUnknownSession -> kNotFound;
// kUnknownPeer -> kHostUnreachable; kBadTicket, kTicketExpired -> kAccessDenied; kFull,
// kRateLimited -> kOutOfResources; kUnsupportedVersion -> kUnsupported; kUnknownBinding is
// answered by binding again and never surfaces. No answer from the service within 500 ms of a
// request is kTimedOut, whatever the call's own timeout.
//
// Threading: every method may be called from any thread; ConnectToPeer() and AcceptPeer() may
// block at the same time on different threads. The implementation refreshes the binding on its
// own until Unbind() or the Transport is destroyed. Two machines that ConnectToPeer() at each
// other at once both succeed with one socket each, and neither call shows up in AcceptPeer().
class Rendezvous {
public:
    virtual ~Rendezvous() = default;

    // Registers this machine with the rendezvous service as `peerId` of `sessionId`, with the
    // lobby's ticket, waiting up to `timeoutMs` for the service's answer. Calling it again
    // replaces the binding (a renewed ticket, or a changed address). Errors: kUnsupported;
    // kInvalidArgument for a malformed id, a zero peer id or a ticket of the wrong size;
    // kAccessDenied if the service rejects the ticket (wrong or expired); kOutOfResources if the
    // service is full or rate-limiting; kTimedOut if the service did not answer.
    virtual Status Bind(std::string_view sessionId, std::uint32_t peerId,
                        const std::uint8_t *ticket, std::size_t ticketBytes,
                        std::uint32_t timeoutMs) = 0;

    // Removes this machine's binding and stops its refresh. Sockets already handed out stay
    // open. Errors: kUnsupported; kWrongState if not bound.
    virtual Status Unbind() = 0;

    // Returns a UDP socket whose datagrams reach the named peer (directly or through the relay),
    // waiting up to `timeoutMs`. Errors: kUnsupported; kWrongState if not bound;
    // kInvalidArgument for a malformed name, or a session other than the bound one; kNotFound if
    // the service does not know the session; kTimedOut if the service did not answer;
    // kHostUnreachable if the peer is not bound or no path to it was found in time, directly or
    // relayed.
    virtual Result<SocketId> ConnectToPeer(std::string_view peerName,
                                           std::uint32_t timeoutMs) = 0;

    // Waits up to `timeoutMs` for a peer of the bound session to reach this machine and returns
    // the UDP socket that talks to it. The peer's name is not reported here; it travels in the
    // session layer's own messages. Errors: kUnsupported; kWrongState if not
    // bound; kTimedOut if no peer arrived; kHostUnreachable if one tried and no path was found.
    virtual Result<SocketId> AcceptPeer(std::uint32_t timeoutMs) = 0;
};

class Transport {
public:
    virtual ~Transport() = default;

    // Creates a socket. Errors: kUnsupported; kOutOfResources.
    virtual Result<SocketId> Open(SocketKind kind) = 0;

    // Closes a socket; a TCP connection is closed as the linger option says. The handle is dead
    // afterwards. Errors: kUnsupported; kInvalidHandle.
    virtual Status Close(SocketId socket) = 0;

    // Sets an option. Errors: kUnsupported; kInvalidHandle; kInvalidArgument for a value out of
    // range, a TCP option on UDP, or kBytesReadable.
    virtual Status SetOption(SocketId socket, SocketOption option, std::int32_t value) = 0;

    // Reads an option. Errors: kUnsupported; kInvalidHandle; kInvalidArgument for a TCP option
    // on UDP.
    virtual Result<std::int32_t> GetOption(SocketId socket, SocketOption option) = 0;

    // Gives the socket a local address. Errors: kUnsupported; kInvalidHandle; kWrongState if
    // already bound; kAddressInUse; kInvalidArgument if the address is not this host's.
    virtual Status Bind(SocketId socket, Endpoint local) = 0;

    // TCP: connects to `peer`. UDP: sets the only peer Send() and Receive() use. Errors:
    // kUnsupported; kInvalidHandle; kInProgress (non-blocking: completion shows in Select() as
    // writable, failure as errored); kConnectionRefused; kTimedOut; kHostUnreachable;
    // kWrongState if a TCP socket is already connected or listening.
    virtual Status Connect(SocketId socket, Endpoint peer) = 0;

    // TCP: starts accepting connections on a bound socket, queueing up to `backlog`. Errors:
    // kUnsupported; kInvalidHandle; kWrongState if unbound, connected, or UDP.
    virtual Status Listen(SocketId socket, std::uint32_t backlog) = 0;

    // TCP: takes the next queued connection. The new socket has the listening socket's options.
    // Errors: kUnsupported; kInvalidHandle; kWrongState if not listening; kWouldBlock.
    virtual Result<Accepted> Accept(SocketId socket) = 0;

    // Sends on a connected socket. TCP may send fewer bytes than given and returns how many;
    // UDP sends one datagram, all or nothing. Errors: kUnsupported; kInvalidHandle;
    // kNotConnected; kWouldBlock; kConnectionReset; kInvalidArgument if a datagram is over
    // 65507 bytes.
    virtual Result<std::size_t> Send(SocketId socket, const void *data, std::size_t bytes) = 0;

    // Receives on a connected socket. TCP: up to `bytes` of the stream, 0 when the peer has shut
    // down sending. UDP: one datagram, truncated to `bytes` (the rest is lost). Errors:
    // kUnsupported; kInvalidHandle; kNotConnected; kWouldBlock; kConnectionReset.
    virtual Result<std::size_t> Receive(SocketId socket, void *out, std::size_t bytes) = 0;

    // UDP: sends one datagram to `peer`; an unbound socket is bound to any address first.
    // Errors: as Send(), and kAccessDenied for a broadcast without kBroadcast set.
    virtual Result<std::size_t> SendTo(SocketId socket, const void *data, std::size_t bytes,
                                       Endpoint peer) = 0;

    // UDP: receives one datagram and its sender. Errors: as Receive(), and kWrongState if the
    // socket is not bound.
    virtual Result<Received> ReceiveFrom(SocketId socket, void *out, std::size_t bytes) = 0;

    // TCP: ends sending, receiving or both without closing. Errors: kUnsupported;
    // kInvalidHandle; kNotConnected.
    virtual Status Shutdown(SocketId socket, ShutdownHow how) = 0;

    // The connected peer's address. Errors: kUnsupported; kInvalidHandle; kNotConnected.
    virtual Result<Endpoint> PeerName(SocketId socket) = 0;

    // The socket's own address (after Bind(), Connect() or a first SendTo()). Errors:
    // kUnsupported; kInvalidHandle; kWrongState if not bound.
    virtual Result<Endpoint> LocalName(SocketId socket) = 0;

    // Waits until at least one socket in `sets` is ready or `timeoutMs` passes (std::nullopt =
    // forever; 0 = poll). Returns the number of ready entries over all three lists, 0 on timeout,
    // and trims the lists to the ready sockets. Errors: kUnsupported; kInvalidHandle if any
    // socket is not live (the lists are then unchanged); kInvalidArgument if all lists are empty.
    virtual Result<std::size_t> Select(SelectSet &sets, std::optional<std::uint32_t> timeoutMs) = 0;

    // The IPv4 addresses of a host name or dotted quad, in the resolver's order. May block for
    // seconds. Errors: kUnsupported; kInvalidArgument for an empty name; kNotFound;
    // kTimedOut; kHostUnreachable if no resolver can be reached.
    virtual Result<std::vector<std::uint32_t>> Resolve(std::string_view hostName) = 0;

    // This machine's host name. Errors: kUnsupported; kIoError.
    virtual Result<std::string> HostName() = 0;

    // The Rendezvous of this transport. The pointer lives as long as the transport. Errors:
    // kUnsupported until Phase 3.
    virtual Result<Rendezvous *> GetRendezvous() = 0;
};

// A Transport whose every method (and Rendezvous) fails with kUnsupported.
std::unique_ptr<Transport> makeTransportStub();

// The build's real backend, on the host sockets (B7); declared here so that the client needs no
// backend header, defined by the backend's own source file once it lands.
std::unique_ptr<Transport> makeTransport();

} // namespace sfcop::platform
