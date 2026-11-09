#include "TcpServer.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

namespace sfcop::auth {
namespace {

bool setNonBlocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

}  // namespace

TcpServer::TcpServer(Factory factory, TcpServerLimits limits)
    : factory_(std::move(factory)), limits_(limits) {}

TcpServer::~TcpServer() {
    for (Connection& c : connections_) ::close(c.fd);
    if (listenFd_ >= 0) ::close(listenFd_);
}

bool TcpServer::listen(const std::string& address, std::uint16_t port, std::string& error) {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        error = "cannot open a TCP socket";
        return false;
    }
    int one = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    ::setsockopt(listenFd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &sa.sin_addr) != 1) {
        error = "not a dotted-quad address: " + address;
        return false;
    }
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0 ||
        ::listen(listenFd_, 64) != 0 || !setNonBlocking(listenFd_)) {
        error = "cannot listen on TCP port " + std::to_string(port);
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    socklen_t len = sizeof sa;
    ::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&sa), &len);
    port_ = ntohs(sa.sin_port);
    return true;
}

void TcpServer::accept(std::uint64_t nowMs) {
    for (int burst = 0; burst < 64; ++burst) {
        sockaddr_in from{};
        socklen_t len = sizeof from;
        int fd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&from), &len);
        if (fd < 0) return;
        if (connections_.size() >= limits_.maxConnections || !setNonBlocking(fd)) {
            ::close(fd);
            continue;
        }
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        std::unique_ptr<StreamSession> session = factory_(ntohl(from.sin_addr.s_addr));
        if (!session) {
            ::close(fd);
            continue;
        }
        Connection c;
        c.fd = fd;
        c.session = std::move(session);
        c.lastActivityMs = nowMs;
        c.pending = c.session->takeOutput();  // a protocol where the server speaks first
        connections_.push_back(std::move(c));
        flush(connections_.back());
    }
}

void TcpServer::flush(Connection& c) {
    while (!c.pending.empty()) {
        ssize_t n = ::send(c.fd, c.pending.data(), c.pending.size(), kSendFlags);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
            c.dead = true;
            return;
        }
        c.pending.erase(0, std::size_t(n));
    }
    if (c.pending.size() > limits_.maxPendingOutput) c.dead = true;
    if (c.pending.empty() && c.session->closing()) c.dead = true;
}

void TcpServer::poll(std::uint32_t timeoutMs, std::uint64_t nowMs) {
    std::vector<pollfd> fds;
    fds.reserve(connections_.size() + 1);
    fds.push_back(pollfd{listenFd_, POLLIN, 0});
    for (const Connection& c : connections_) {
        short events = POLLIN;
        if (!c.pending.empty()) events |= POLLOUT;
        fds.push_back(pollfd{c.fd, events, 0});
    }
    int ready = ::poll(fds.data(), nfds_t(fds.size()), int(timeoutMs));
    if (ready > 0) {
        // Connections first, by index, before accept() grows the list.
        for (std::size_t i = 0; i < connections_.size(); ++i) {
            Connection& c = connections_[i];
            short revents = fds[i + 1].revents;
            if (revents & (POLLERR | POLLNVAL)) {
                c.dead = true;
                continue;
            }
            if (revents & (POLLIN | POLLHUP)) {
                std::uint8_t buffer[4096];
                ssize_t n = ::recv(c.fd, buffer, sizeof buffer, 0);
                if (n > 0) {
                    c.lastActivityMs = nowMs;
                    c.session->onBytes(buffer, std::size_t(n));
                    c.pending += c.session->takeOutput();
                } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    c.dead = true;
                    continue;
                }
            }
            flush(c);
        }
        if (fds[0].revents & POLLIN) accept(nowMs);
    }
    for (Connection& c : connections_) {
        if (nowMs - c.lastActivityMs > limits_.idleMs) c.dead = true;
    }
    auto end = std::remove_if(connections_.begin(), connections_.end(), [](Connection& c) {
        if (c.dead) ::close(c.fd);
        return c.dead;
    });
    connections_.erase(end, connections_.end());
}

}  // namespace sfcop::auth
