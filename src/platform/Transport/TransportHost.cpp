// The host backend of Transport on BSD sockets (macOS and Linux). Windows has its own file,
// TransportHost_windows.cpp; this whole file is for the BSD socket API only.
//
// Every operating-system socket is non-blocking underneath. A socket the caller leaves blocking
// (the default, kNonBlocking 0) is made to wait with poll(), on the socket and on a wake pipe of
// its own; Close() writes to that pipe, so a call blocked on the socket in another thread returns
// kNotConnected at once, as Transport.h requires. The descriptor itself is closed when the last
// call using it has returned.
//
// Each TransportHost owns its own socket table; nothing is process-wide, so several transports
// (the game's and, later, the peers of DESIGN §5.11) live side by side.
#if !defined(_WIN32)

#include "platform/Transport/Transport.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <mutex>
#include <strings.h>
#include <unordered_map>

namespace sfcop::platform {
namespace {

// Which call an errno came from, where the same errno means different things.
enum class Op : std::uint8_t { kAny, kOpen, kBind, kConnect, kListen, kAccept, kTransfer };

struct ErrnoMapping {
    Op op;
    int code;
    Error error;
};

// errno to Error, the whole mapping. A row for a specific call wins over a kAny row.
constexpr ErrnoMapping kErrnoTable[] = {
    {Op::kAny, EAGAIN, Error::kWouldBlock},
#if EWOULDBLOCK != EAGAIN
    {Op::kAny, EWOULDBLOCK, Error::kWouldBlock},
#endif
    {Op::kAny, EINPROGRESS, Error::kInProgress},
    {Op::kAny, EALREADY, Error::kInProgress},
    {Op::kAny, EISCONN, Error::kWrongState},
    {Op::kAny, ENOTCONN, Error::kNotConnected},
    {Op::kAny, EDESTADDRREQ, Error::kNotConnected},
    {Op::kAny, EPIPE, Error::kConnectionReset},
    {Op::kAny, ECONNRESET, Error::kConnectionReset},
    {Op::kAny, ECONNABORTED, Error::kConnectionReset},
    {Op::kAny, ECONNREFUSED, Error::kConnectionRefused},
    {Op::kTransfer, ECONNREFUSED, Error::kConnectionReset}, // UDP: an ICMP port unreachable
    {Op::kAny, ETIMEDOUT, Error::kTimedOut},
    {Op::kAny, EHOSTUNREACH, Error::kHostUnreachable},
    {Op::kAny, ENETUNREACH, Error::kHostUnreachable},
    {Op::kAny, ENETDOWN, Error::kHostUnreachable},
    {Op::kAny, EHOSTDOWN, Error::kHostUnreachable},
    {Op::kAny, EADDRINUSE, Error::kAddressInUse},
    {Op::kAny, EADDRNOTAVAIL, Error::kInvalidArgument},
    {Op::kAny, EACCES, Error::kAccessDenied},
    {Op::kAny, EPERM, Error::kAccessDenied},
    {Op::kAny, EBADF, Error::kInvalidHandle},
    {Op::kAny, ENOTSOCK, Error::kInvalidHandle},
    {Op::kAny, EMFILE, Error::kOutOfResources},
    {Op::kAny, ENFILE, Error::kOutOfResources},
    {Op::kAny, ENOBUFS, Error::kOutOfResources},
    {Op::kAny, ENOMEM, Error::kOutOfResources},
    {Op::kOpen, EACCES, Error::kOutOfResources},
    {Op::kOpen, EAFNOSUPPORT, Error::kOutOfResources},
    {Op::kOpen, EPROTONOSUPPORT, Error::kOutOfResources},
    {Op::kAny, EMSGSIZE, Error::kInvalidArgument},
    {Op::kAny, EINVAL, Error::kInvalidArgument},
    {Op::kBind, EINVAL, Error::kWrongState}, // already bound
    {Op::kListen, EINVAL, Error::kWrongState},
    {Op::kAccept, EINVAL, Error::kWrongState}, // not listening
    {Op::kAny, EOPNOTSUPP, Error::kWrongState},
    {Op::kAny, ENOPROTOOPT, Error::kInvalidArgument},
};

Error fromErrno(Op op, int code) {
    Error general = Error::kIoError;
    for (const ErrnoMapping &row : kErrnoTable) {
        if (row.code != code) {
            continue;
        }
        if (row.op == op) {
            return row.error;
        }
        if (row.op == Op::kAny) {
            general = row.error;
        }
    }
    return general;
}

sockaddr_in toSockaddr(Endpoint endpoint) {
    sockaddr_in out{};
#if defined(__APPLE__)
    out.sin_len = sizeof out;
#endif
    out.sin_family = AF_INET;
    out.sin_port = htons(endpoint.port);
    out.sin_addr.s_addr = htonl(endpoint.address);
    return out;
}

Endpoint fromSockaddr(const sockaddr_in &in) {
    return Endpoint{ntohl(in.sin_addr.s_addr), ntohs(in.sin_port)};
}

bool setFlag(int fd, int command, int getCommand, int flag) {
    int flags = ::fcntl(fd, getCommand);
    return flags >= 0 && ::fcntl(fd, command, flags | flag) == 0;
}

bool prepareDescriptor(int fd) {
    if (!setFlag(fd, F_SETFL, F_GETFL, O_NONBLOCK) || !setFlag(fd, F_SETFD, F_GETFD, FD_CLOEXEC)) {
        return false;
    }
#if defined(SO_NOSIGPIPE)
    int one = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) != 0) {
        return false;
    }
#endif
    return true;
}

#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0; // SO_NOSIGPIPE is set on the socket instead
#endif

#if defined(SO_LINGER_SEC)
constexpr int kLingerOption = SO_LINGER_SEC; // Darwin's SO_LINGER counts clock ticks
#else
constexpr int kLingerOption = SO_LINGER;
#endif

constexpr std::size_t kMaxDatagram = 65507;

// One socket. Shared by the table and by every call in flight on it.
struct HostSocket {
    int fd = -1;
    int wakeRead = -1;
    int wakeWrite = -1;
    SocketKind kind = SocketKind::kUdp;
    std::atomic<bool> nonBlocking{false};
    std::atomic<bool> closing{false};
    std::atomic<bool> listening{false};
    std::atomic<bool> udpConnected{false};
    std::atomic<bool> connecting{false}; // a non-blocking TCP connect has not finished

    HostSocket() = default;
    HostSocket(const HostSocket &) = delete;
    HostSocket &operator=(const HostSocket &) = delete;

    ~HostSocket() {
        if (fd >= 0) {
            if (!nonBlocking.load()) {
                // A blocking socket's close waits as its linger option says; Darwin does not
                // wait on a non-blocking descriptor, so make it blocking for the close.
                int flags = ::fcntl(fd, F_GETFL);
                if (flags >= 0) {
                    (void)::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
                }
            }
            ::close(fd);
        }
        if (wakeRead >= 0) {
            ::close(wakeRead);
        }
        if (wakeWrite >= 0) {
            ::close(wakeWrite);
        }
    }
};

using SocketPtr = std::shared_ptr<HostSocket>;

class TransportHost final : public Transport {
public:
    Result<SocketId> Open(SocketKind kind) override {
        auto socket = std::make_shared<HostSocket>();
        socket->kind = kind;
        socket->fd = ::socket(AF_INET, kind == SocketKind::kTcp ? SOCK_STREAM : SOCK_DGRAM, 0);
        if (socket->fd < 0) {
            return fromErrno(Op::kOpen, errno);
        }
        int wake[2];
        if (::pipe(wake) != 0) {
            return Error::kOutOfResources;
        }
        socket->wakeRead = wake[0];
        socket->wakeWrite = wake[1];
        if (!prepareDescriptor(socket->fd) ||
            !setFlag(wake[0], F_SETFD, F_GETFD, FD_CLOEXEC) ||
            !setFlag(wake[1], F_SETFD, F_GETFD, FD_CLOEXEC) ||
            !setFlag(wake[1], F_SETFL, F_GETFL, O_NONBLOCK)) {
            return Error::kOutOfResources;
        }
        if (kind == SocketKind::kUdp) {
            // Room for the largest datagram (Darwin's default send buffer limits a datagram to
            // 9216 bytes).
            int size = 0;
            socklen_t length = sizeof size;
            if (::getsockopt(socket->fd, SOL_SOCKET, SO_SNDBUF, &size, &length) == 0 &&
                size < 65536) {
                size = 65536;
                (void)::setsockopt(socket->fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof size);
            }
        }
        return add(std::move(socket));
    }

    Status Close(SocketId id) override {
        SocketPtr socket;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto found = sockets_.find(id.value);
            if (found == sockets_.end()) {
                return Error::kInvalidHandle;
            }
            socket = std::move(found->second);
            sockets_.erase(found);
        }
        socket->closing.store(true);
        const char byte = 1;
        (void)!::write(socket->wakeWrite, &byte, 1);
        return {};
    }

    Status SetOption(SocketId id, SocketOption option, std::int32_t value) override {
        SocketPtr socket = find(id);
        if (!socket) {
            return Error::kInvalidHandle;
        }
        const bool tcp = socket->kind == SocketKind::kTcp;
        auto boolean = [&](int level, int name) -> Status {
            if (value != 0 && value != 1) {
                return Error::kInvalidArgument;
            }
            int v = value;
            if (::setsockopt(socket->fd, level, name, &v, sizeof v) != 0) {
                return fromErrno(Op::kAny, errno);
            }
            return {};
        };
        switch (option) {
        case SocketOption::kNonBlocking:
            if (value != 0 && value != 1) {
                return Error::kInvalidArgument;
            }
            socket->nonBlocking.store(value == 1);
            return {};
        case SocketOption::kBroadcast:
            return boolean(SOL_SOCKET, SO_BROADCAST);
        case SocketOption::kReuseAddress: {
            Status reuse = boolean(SOL_SOCKET, SO_REUSEADDR);
#if defined(SO_REUSEPORT)
            // Winsock's SO_REUSEADDR lets two UDP sockets share a port; BSD needs SO_REUSEPORT.
            // Phase 3 note: on Linux, SO_REUSEPORT also load-balances unicast datagrams across
            // the sockets sharing the port, which Winsock does not (there the last bind wins).
            if (reuse.Ok() && !tcp) {
                reuse = boolean(SOL_SOCKET, SO_REUSEPORT);
            }
#endif
            return reuse;
        }
        case SocketOption::kNoDelay:
            if (!tcp) {
                return Error::kInvalidArgument;
            }
            return boolean(IPPROTO_TCP, TCP_NODELAY);
        case SocketOption::kKeepAlive:
            if (!tcp) {
                return Error::kInvalidArgument;
            }
            return boolean(SOL_SOCKET, SO_KEEPALIVE);
        case SocketOption::kReceiveBuffer:
        case SocketOption::kSendBuffer: {
            if (value <= 0) {
                return Error::kInvalidArgument;
            }
            int v = value;
            int name = option == SocketOption::kReceiveBuffer ? SO_RCVBUF : SO_SNDBUF;
            if (::setsockopt(socket->fd, SOL_SOCKET, name, &v, sizeof v) != 0) {
                return fromErrno(Op::kAny, errno);
            }
            return {};
        }
        case SocketOption::kLingerSeconds: {
            if (!tcp || value < -1 || value > 65535) {
                return Error::kInvalidArgument;
            }
            linger l{};
            l.l_onoff = value >= 0 ? 1 : 0;
            l.l_linger = value >= 0 ? value : 0;
            if (::setsockopt(socket->fd, SOL_SOCKET, kLingerOption, &l, sizeof l) != 0) {
                return fromErrno(Op::kAny, errno);
            }
            return {};
        }
        case SocketOption::kBytesReadable:
            return Error::kInvalidArgument;
        }
        return Error::kInvalidArgument;
    }

    Result<std::int32_t> GetOption(SocketId id, SocketOption option) override {
        SocketPtr socket = find(id);
        if (!socket) {
            return Error::kInvalidHandle;
        }
        const bool tcp = socket->kind == SocketKind::kTcp;
        auto read = [&](int level, int name, bool boolean) -> Result<std::int32_t> {
            int v = 0;
            socklen_t length = sizeof v;
            if (::getsockopt(socket->fd, level, name, &v, &length) != 0) {
                return fromErrno(Op::kAny, errno);
            }
            return boolean ? std::int32_t{v != 0} : std::int32_t{v};
        };
        switch (option) {
        case SocketOption::kNonBlocking:
            return std::int32_t{socket->nonBlocking.load()};
        case SocketOption::kBroadcast:
            return read(SOL_SOCKET, SO_BROADCAST, true);
        case SocketOption::kReuseAddress:
            return read(SOL_SOCKET, SO_REUSEADDR, true);
        case SocketOption::kNoDelay:
            if (!tcp) {
                return Error::kInvalidArgument;
            }
            return read(IPPROTO_TCP, TCP_NODELAY, true);
        case SocketOption::kKeepAlive:
            if (!tcp) {
                return Error::kInvalidArgument;
            }
            return read(SOL_SOCKET, SO_KEEPALIVE, true);
        case SocketOption::kReceiveBuffer:
            return read(SOL_SOCKET, SO_RCVBUF, false);
        case SocketOption::kSendBuffer:
            return read(SOL_SOCKET, SO_SNDBUF, false);
        case SocketOption::kLingerSeconds: {
            if (!tcp) {
                return Error::kInvalidArgument;
            }
            linger l{};
            socklen_t length = sizeof l;
            if (::getsockopt(socket->fd, SOL_SOCKET, kLingerOption, &l, &length) != 0) {
                return fromErrno(Op::kAny, errno);
            }
            return l.l_onoff ? std::int32_t{l.l_linger} : std::int32_t{-1};
        }
        case SocketOption::kBytesReadable: {
#if defined(SO_NREAD)
            // Darwin's FIONREAD counts every queued datagram; SO_NREAD counts the first, which
            // is what one Receive() returns.
            if (!tcp) {
                return read(SOL_SOCKET, SO_NREAD, false);
            }
#endif
            int n = 0;
            if (::ioctl(socket->fd, FIONREAD, &n) != 0) {
                return fromErrno(Op::kAny, errno);
            }
            return std::int32_t{n};
        }
        }
        return Error::kInvalidArgument;
    }

    Status Bind(SocketId id, Endpoint local) override {
        SocketPtr socket = find(id);
        if (!socket) {
            return Error::kInvalidHandle;
        }
        if (bound(*socket)) {
            return Error::kWrongState;
        }
        sockaddr_in address = toSockaddr(local);
        if (::bind(socket->fd, reinterpret_cast<const sockaddr *>(&address), sizeof address) != 0) {
            return fromErrno(Op::kBind, errno);
        }
        return {};
    }

    Status Connect(SocketId id, Endpoint peer) override {
        SocketPtr socket = find(id);
        if (!socket) {
            return Error::kInvalidHandle;
        }
        sockaddr_in address = toSockaddr(peer);
        const auto *raw = reinterpret_cast<const sockaddr *>(&address);
        if (socket->kind == SocketKind::kUdp) {
            if (::connect(socket->fd, raw, sizeof address) != 0) {
                return fromErrno(Op::kConnect, errno);
            }
            socket->udpConnected.store(true);
            return {};
        }
        if (socket->listening.load()) {
            return Error::kWrongState;
        }
        if (socket->connecting.load()) {
            int failure = 0;
            if (!connectFinished(*socket, failure)) {
                return Error::kInProgress;
            }
            return failure == 0 ? Status{Error::kWrongState}
                                : Status{fromErrno(Op::kConnect, failure)};
        }
        if (::connect(socket->fd, raw, sizeof address) == 0) {
            return {};
        }
        int code = errno;
        if (code != EINPROGRESS && code != EINTR) {
            return fromErrno(Op::kConnect, code);
        }
        if (socket->nonBlocking.load()) {
            socket->connecting.store(true);
            return Error::kInProgress;
        }
        for (;;) {
            Status ready = waitFor(*socket, POLLOUT, std::nullopt);
            if (!ready.Ok()) {
                return ready;
            }
            int failure = 0;
            if (connectFinished(*socket, failure)) {
                return failure == 0 ? Status{} : Status{fromErrno(Op::kConnect, failure)};
            }
        }
    }

    Status Listen(SocketId id, std::uint32_t backlog) override {
        SocketPtr socket = find(id);
        if (!socket) {
            return Error::kInvalidHandle;
        }
        if (socket->kind != SocketKind::kTcp || !bound(*socket) || hasPeer(*socket)) {
            return Error::kWrongState;
        }
        int depth = static_cast<int>((std::min)(backlog, static_cast<std::uint32_t>(SOMAXCONN)));
        if (::listen(socket->fd, depth) != 0) {
            return fromErrno(Op::kListen, errno);
        }
        socket->listening.store(true);
        return {};
    }

    Result<Accepted> Accept(SocketId id) override {
        SocketPtr socket = find(id);
        if (!socket) {
            return Error::kInvalidHandle;
        }
        if (!socket->listening.load()) {
            return Error::kWrongState;
        }
        sockaddr_in peer{};
        int fd = -1;
        Status done = blocking(*socket, POLLIN, Op::kAccept, [&]() -> long {
            socklen_t length = sizeof peer;
            fd = ::accept(socket->fd, reinterpret_cast<sockaddr *>(&peer), &length);
            return fd;
        });
        if (!done.Ok()) {
            return done.Code();
        }
        auto accepted = std::make_shared<HostSocket>();
        accepted->fd = fd;
        accepted->kind = SocketKind::kTcp;
        accepted->nonBlocking.store(socket->nonBlocking.load());
        int wake[2];
        if (::pipe(wake) != 0) {
            return Error::kOutOfResources;
        }
        accepted->wakeRead = wake[0];
        accepted->wakeWrite = wake[1];
        if (!prepareDescriptor(fd) || !setFlag(wake[0], F_SETFD, F_GETFD, FD_CLOEXEC) ||
            !setFlag(wake[1], F_SETFD, F_GETFD, FD_CLOEXEC) ||
            !setFlag(wake[1], F_SETFL, F_GETFL, O_NONBLOCK)) {
            return Error::kOutOfResources;
        }
        copyOption(socket->fd, fd, IPPROTO_TCP, TCP_NODELAY);
        copyOption(socket->fd, fd, SOL_SOCKET, SO_KEEPALIVE);
        linger l{};
        socklen_t length = sizeof l;
        if (::getsockopt(socket->fd, SOL_SOCKET, kLingerOption, &l, &length) == 0) {
            (void)::setsockopt(fd, SOL_SOCKET, kLingerOption, &l, sizeof l);
        }
        Result<SocketId> added = add(std::move(accepted));
        if (!added.Ok()) {
            return added.Code();
        }
        return Accepted{added.Value(), fromSockaddr(peer)};
    }

    Result<std::size_t> Send(SocketId id, const void *data, std::size_t bytes) override {
        SocketPtr socket = find(id);
        if (!socket) {
            return Error::kInvalidHandle;
        }
        if (socket->kind == SocketKind::kUdp) {
            if (!socket->udpConnected.load()) {
                return Error::kNotConnected;
            }
            if (bytes > kMaxDatagram) {
                return Error::kInvalidArgument;
            }
        }
        return transfer(*socket, POLLOUT, data, bytes, [&](const char *from, std::size_t n) {
            return static_cast<long>(::send(socket->fd, from, n, kSendFlags));
        });
    }

    Result<std::size_t> Receive(SocketId id, void *out, std::size_t bytes) override {
        SocketPtr socket = find(id);
        if (!socket) {
            return Error::kInvalidHandle;
        }
        if (socket->kind == SocketKind::kUdp && !socket->udpConnected.load()) {
            return Error::kNotConnected;
        }
        std::size_t got = 0;
        Status done = blocking(*socket, POLLIN, Op::kTransfer, [&]() -> long {
            long n = static_cast<long>(::recv(socket->fd, out, bytes, 0));
            got = n > 0 ? static_cast<std::size_t>(n) : 0;
            return n;
        });
        if (!done.Ok()) {
            return done.Code();
        }
        return got;
    }

    Result<std::size_t> SendTo(SocketId id, const void *data, std::size_t bytes,
                               Endpoint peer) override {
        SocketPtr socket = find(id);
        if (!socket) {
            return Error::kInvalidHandle;
        }
        if (socket->kind == SocketKind::kTcp) {
            // A stream socket ignores the address, as Winsock's sendto does.
            return transfer(*socket, POLLOUT, data, bytes, [&](const char *from, std::size_t n) {
                return static_cast<long>(::send(socket->fd, from, n, kSendFlags));
            });
        }
        if (bytes > kMaxDatagram) {
            return Error::kInvalidArgument;
        }
        sockaddr_in address = toSockaddr(peer);
        return transfer(*socket, POLLOUT, data, bytes, [&](const char *from, std::size_t n) {
            return static_cast<long>(::sendto(socket->fd, from, n, kSendFlags,
                                              reinterpret_cast<const sockaddr *>(&address),
                                              sizeof address));
        });
    }

    Result<Received> ReceiveFrom(SocketId id, void *out, std::size_t bytes) override {
        SocketPtr socket = find(id);
        if (!socket) {
            return Error::kInvalidHandle;
        }
        if (socket->kind == SocketKind::kUdp && !bound(*socket)) {
            return Error::kWrongState;
        }
        sockaddr_in from{};
        std::size_t got = 0;
        Status done = blocking(*socket, POLLIN, Op::kTransfer, [&]() -> long {
            socklen_t length = sizeof from;
            long n = static_cast<long>(
                ::recvfrom(socket->fd, out, bytes, 0, reinterpret_cast<sockaddr *>(&from), &length));
            got = n > 0 ? static_cast<std::size_t>(n) : 0;
            return n;
        });
        if (!done.Ok()) {
            return done.Code();
        }
        Endpoint sender = fromSockaddr(from);
        if (socket->kind == SocketKind::kTcp) {
            sockaddr_in peer{};
            socklen_t length = sizeof peer;
            if (::getpeername(socket->fd, reinterpret_cast<sockaddr *>(&peer), &length) == 0) {
                sender = fromSockaddr(peer);
            }
        }
        return Received{got, sender};
    }

    Status Shutdown(SocketId id, ShutdownHow how) override {
        SocketPtr socket = find(id);
        if (!socket) {
            return Error::kInvalidHandle;
        }
        int mode = how == ShutdownHow::kReceive ? SHUT_RD
                   : how == ShutdownHow::kSend  ? SHUT_WR
                                                : SHUT_RDWR;
        if (::shutdown(socket->fd, mode) != 0) {
            return fromErrno(Op::kAny, errno);
        }
        return {};
    }

    Result<Endpoint> PeerName(SocketId id) override {
        SocketPtr socket = find(id);
        if (!socket) {
            return Error::kInvalidHandle;
        }
        sockaddr_in peer{};
        socklen_t length = sizeof peer;
        if (::getpeername(socket->fd, reinterpret_cast<sockaddr *>(&peer), &length) != 0) {
            return Error::kNotConnected;
        }
        return fromSockaddr(peer);
    }

    Result<Endpoint> LocalName(SocketId id) override {
        SocketPtr socket = find(id);
        if (!socket) {
            return Error::kInvalidHandle;
        }
        sockaddr_in local{};
        socklen_t length = sizeof local;
        if (::getsockname(socket->fd, reinterpret_cast<sockaddr *>(&local), &length) != 0) {
            return fromErrno(Op::kAny, errno);
        }
        if (local.sin_port == 0) {
            return Error::kWrongState;
        }
        return fromSockaddr(local);
    }

    Result<std::size_t> Select(SelectSet &sets, std::optional<std::uint32_t> timeoutMs) override {
        if (sets.readable.empty() && sets.writable.empty() && sets.errored.empty()) {
            return Error::kInvalidArgument;
        }
        // Each list entry gets one pollfd; each socket's wake pipe one more.
        struct Watched {
            SocketPtr socket;
            int list; // 0 readable, 1 writable, 2 errored
        };
        std::vector<Watched> watched;
        std::vector<SocketPtr> distinct;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const std::vector<SocketId> *lists[3] = {&sets.readable, &sets.writable, &sets.errored};
            for (int list = 0; list < 3; ++list) {
                for (SocketId id : *lists[list]) {
                    auto found = sockets_.find(id.value);
                    if (found == sockets_.end()) {
                        return Error::kInvalidHandle;
                    }
                    watched.push_back(Watched{found->second, list});
                    if (std::find(distinct.begin(), distinct.end(), found->second) ==
                        distinct.end()) {
                        distinct.push_back(found->second);
                    }
                }
            }
        }
        std::vector<pollfd> polls;
        for (const Watched &w : watched) {
            short events = w.list == 0 ? POLLIN : w.list == 1 ? POLLOUT : POLLPRI;
            if (w.list != 0 && w.socket->connecting.load()) {
                events = POLLOUT; // a connect's end shows as writable, failed or not
            }
            polls.push_back(pollfd{w.socket->fd, events, 0});
        }
        for (const SocketPtr &socket : distinct) {
            polls.push_back(pollfd{socket->wakeRead, POLLIN, 0});
        }
        int ready = pollUntil(polls, timeoutMs);
        if (ready < 0) {
            return fromErrno(Op::kAny, errno);
        }
        for (const SocketPtr &socket : distinct) {
            if (socket->closing.load()) {
                return Error::kNotConnected;
            }
        }
        // A connect that ended: writable if it succeeded, errored if it failed (Winsock's rule).
        std::vector<std::pair<HostSocket *, int>> ended;
        for (std::size_t i = 0; i < watched.size(); ++i) {
            HostSocket &socket = *watched[i].socket;
            bool seen = std::any_of(ended.begin(), ended.end(),
                                    [&](const auto &e) { return e.first == &socket; });
            if (!seen && socket.connecting.load() && polls[i].revents != 0) {
                int failure = 0;
                if (connectFinished(socket, failure)) {
                    ended.emplace_back(&socket, failure);
                }
            }
        }
        auto connectResult = [&](HostSocket &socket) -> int {
            for (const auto &e : ended) {
                if (e.first == &socket) {
                    return e.second;
                }
            }
            return -1; // not a connect that ended in this call
        };
        SelectSet result;
        std::vector<SocketId> *lists[3] = {&result.readable, &result.writable, &result.errored};
        const std::vector<SocketId> *original[3] = {&sets.readable, &sets.writable,
                                                    &sets.errored};
        std::size_t index = 0;
        std::size_t total = 0;
        for (int list = 0; list < 3; ++list) {
            for (SocketId id : *original[list]) {
                const Watched &w = watched[index];
                short revents = polls[index].revents;
                ++index;
                int connect = connectResult(*w.socket);
                bool hit = false;
                if (list == 0) {
                    hit = (revents & (POLLIN | POLLHUP | POLLERR)) != 0 && connect < 0;
                } else if (list == 1) {
                    hit = connect >= 0 ? connect == 0 : (revents & POLLOUT) != 0;
                } else {
                    hit = connect >= 0 ? connect != 0 : (revents & POLLPRI) != 0;
                }
                if (hit) {
                    lists[list]->push_back(id);
                    ++total;
                }
            }
        }
        sets = std::move(result);
        return total;
    }

    Result<std::vector<std::uint32_t>> Resolve(std::string_view hostName) override {
        if (hostName.empty()) {
            return Error::kInvalidArgument;
        }
        std::string name(hostName);
        in_addr literal{};
        if (::inet_pton(AF_INET, name.c_str(), &literal) == 1) {
            return std::vector<std::uint32_t>{ntohl(literal.s_addr)};
        }
        // This machine's own name is its interfaces' addresses, as on Windows, without asking a
        // resolver (a Mac's own name is often unknown to it).
        Result<std::string> self = HostName();
        if (self.Ok() && ::strcasecmp(self.Value().c_str(), name.c_str()) == 0) {
            return interfaceAddresses();
        }
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *list = nullptr;
        int code = ::getaddrinfo(name.c_str(), nullptr, &hints, &list);
        if (code != 0) {
            switch (code) {
            case EAI_AGAIN:
                return Error::kTimedOut;
            case EAI_FAIL:
                return Error::kHostUnreachable;
            case EAI_MEMORY:
                return Error::kOutOfResources;
            default:
                return Error::kNotFound;
            }
        }
        std::vector<std::uint32_t> addresses;
        for (addrinfo *entry = list; entry; entry = entry->ai_next) {
            if (entry->ai_family != AF_INET || !entry->ai_addr) {
                continue;
            }
            std::uint32_t address =
                ntohl(reinterpret_cast<const sockaddr_in *>(entry->ai_addr)->sin_addr.s_addr);
            if (std::find(addresses.begin(), addresses.end(), address) == addresses.end()) {
                addresses.push_back(address);
            }
        }
        ::freeaddrinfo(list);
        if (addresses.empty()) {
            return Error::kNotFound;
        }
        return addresses;
    }

    Result<std::string> HostName() override {
        char name[256] = {};
        if (::gethostname(name, sizeof name - 1) != 0) {
            return Error::kIoError;
        }
        return std::string(name);
    }

    Result<Rendezvous *> GetRendezvous() override { return Error::kUnsupported; }

private:
    std::mutex mutex_;
    std::unordered_map<std::uint32_t, SocketPtr> sockets_;
    std::uint32_t nextId_ = 1;

    Result<SocketId> add(SocketPtr socket) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (;;) {
            std::uint32_t id = nextId_++;
            if (id != 0 && sockets_.find(id) == sockets_.end()) {
                sockets_.emplace(id, std::move(socket));
                return SocketId{id};
            }
        }
    }

    SocketPtr find(SocketId id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = sockets_.find(id.value);
        return found == sockets_.end() ? nullptr : found->second;
    }

    static bool bound(const HostSocket &socket) {
        sockaddr_in local{};
        socklen_t length = sizeof local;
        return ::getsockname(socket.fd, reinterpret_cast<sockaddr *>(&local), &length) == 0 &&
               local.sin_port != 0;
    }

    static bool hasPeer(const HostSocket &socket) {
        sockaddr_in peer{};
        socklen_t length = sizeof peer;
        return ::getpeername(socket.fd, reinterpret_cast<sockaddr *>(&peer), &length) == 0;
    }

    static void copyOption(int from, int to, int level, int name) {
        int v = 0;
        socklen_t length = sizeof v;
        if (::getsockopt(from, level, name, &v, &length) == 0) {
            (void)::setsockopt(to, level, name, &v, sizeof v);
        }
    }

    // A TCP connect in progress has ended if the socket is writable or failed; `failure` is its
    // errno (0 on success). Clears `connecting` when it has ended.
    static bool connectFinished(HostSocket &socket, int &failure) {
        pollfd p{socket.fd, POLLOUT, 0};
        if (::poll(&p, 1, 0) <= 0 || (p.revents & (POLLOUT | POLLERR | POLLHUP)) == 0) {
            return false;
        }
        int error = 0;
        socklen_t length = sizeof error;
        if (::getsockopt(socket.fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
            error = errno;
        }
        if (error == 0 && !hasPeer(socket)) {
            error = ECONNREFUSED;
        }
        failure = error;
        socket.connecting.store(false);
        return true;
    }

    // poll() that waits up to timeoutMs in all (nullopt: forever), across interruptions.
    static int pollUntil(std::vector<pollfd> &polls, std::optional<std::uint32_t> timeoutMs) {
        using Clock = std::chrono::steady_clock;
        const Clock::time_point deadline =
            Clock::now() + std::chrono::milliseconds(timeoutMs.value_or(0));
        for (;;) {
            int wait = -1;
            if (timeoutMs) {
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                deadline - Clock::now())
                                .count();
                wait = static_cast<int>(std::clamp<long long>(left, 0, INT_MAX));
            }
            int ready = ::poll(polls.data(), static_cast<nfds_t>(polls.size()), wait);
            if (ready >= 0 || errno != EINTR) {
                return ready;
            }
        }
    }

    // Waits until the socket shows `events` or is closed (kNotConnected).
    static Status waitFor(HostSocket &socket, short events, std::optional<std::uint32_t> timeoutMs) {
        std::vector<pollfd> polls{pollfd{socket.fd, events, 0},
                                  pollfd{socket.wakeRead, POLLIN, 0}};
        int ready = pollUntil(polls, timeoutMs);
        if (socket.closing.load()) {
            return Error::kNotConnected;
        }
        if (ready < 0) {
            return fromErrno(Op::kAny, errno);
        }
        return {};
    }

    // Runs `attempt` (a system call returning -1 and errno on failure) until it succeeds,
    // waiting for `events` while the socket is blocking.
    template <typename Attempt>
    static Status blocking(HostSocket &socket, short events, Op op, Attempt attempt) {
        for (;;) {
            if (socket.closing.load()) {
                return Error::kNotConnected;
            }
            if (attempt() >= 0) {
                return {};
            }
            int code = errno;
            if (code == EINTR) {
                continue;
            }
            if (code != EAGAIN && code != EWOULDBLOCK) {
                return fromErrno(op, code);
            }
            if (socket.nonBlocking.load()) {
                return Error::kWouldBlock;
            }
            Status ready = waitFor(socket, events, std::nullopt);
            if (!ready.Ok()) {
                return ready;
            }
        }
    }

    // Sends through `attempt`; a blocking stream socket sends everything, as Winsock does, and
    // reports what went before an error.
    template <typename Attempt>
    static Result<std::size_t> transfer(HostSocket &socket, short events, const void *data,
                                        std::size_t bytes, Attempt attempt) {
        const char *from = static_cast<const char *>(data);
        std::size_t sent = 0;
        do {
            long n = 0;
            Status done = blocking(socket, events, Op::kTransfer, [&]() -> long {
                n = attempt(from + sent, bytes - sent);
                return n;
            });
            if (!done.Ok()) {
                if (sent > 0) {
                    return sent;
                }
                return done.Code();
            }
            sent += static_cast<std::size_t>(n);
        } while (socket.kind == SocketKind::kTcp && !socket.nonBlocking.load() && sent < bytes);
        return sent;
    }

    static Result<std::vector<std::uint32_t>> interfaceAddresses() {
        std::vector<std::uint32_t> addresses;
        ifaddrs *list = nullptr;
        if (::getifaddrs(&list) == 0) {
            for (ifaddrs *entry = list; entry; entry = entry->ifa_next) {
                if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET ||
                    (entry->ifa_flags & IFF_UP) == 0 || (entry->ifa_flags & IFF_LOOPBACK) != 0) {
                    continue;
                }
                std::uint32_t address = ntohl(
                    reinterpret_cast<const sockaddr_in *>(entry->ifa_addr)->sin_addr.s_addr);
                if (std::find(addresses.begin(), addresses.end(), address) == addresses.end()) {
                    addresses.push_back(address);
                }
            }
            ::freeifaddrs(list);
        }
        if (addresses.empty()) {
            addresses.push_back(0x7F000001); // 127.0.0.1
        }
        return addresses;
    }
};

} // namespace

std::unique_ptr<Transport> makeTransport() { return std::make_unique<TransportHost>(); }

} // namespace sfcop::platform

#endif // !defined(_WIN32)
