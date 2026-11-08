#include "UdpSocket.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace sfcop::relay {

UdpSocket::~UdpSocket() {
    if (fd_ >= 0) ::close(fd_);
}

bool UdpSocket::open(const std::string& address, std::uint16_t port, std::string& error) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        error = "cannot create the UDP socket";
        return false;
    }
    int buffer = 4 * 1024 * 1024;  // absorbs a burst while the loop is busy; the kernel may clamp
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof buffer);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &sa.sin_addr) != 1 ||
        ::bind(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
        error = "cannot bind the UDP socket";
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    socklen_t len = sizeof sa;
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&sa), &len) == 0) localPort_ = ntohs(sa.sin_port);
    return true;
}

int UdpSocket::receive(Endpoint& from, std::uint8_t* buffer, std::size_t capacity, int timeoutMs) {
    pollfd p{fd_, POLLIN, 0};
    int r = ::poll(&p, 1, timeoutMs);
    if (r == 0) return 0;
    if (r < 0) return errno == EINTR ? 0 : -1;
    sockaddr_in sa{};
    socklen_t len = sizeof sa;
    ssize_t n = ::recvfrom(fd_, buffer, capacity, 0, reinterpret_cast<sockaddr*>(&sa), &len);
    if (n < 0) return (errno == EINTR || errno == EAGAIN || errno == ECONNREFUSED) ? 0 : -1;
    from.address = ntohl(sa.sin_addr.s_addr);
    from.port = ntohs(sa.sin_port);
    return int(n);
}

void UdpSocket::send(const Endpoint& to, const std::uint8_t* data, std::size_t size) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(to.port);
    sa.sin_addr.s_addr = htonl(to.address);
    // A full send buffer drops the datagram, as the network would; the envelope's own rules
    // (PROTOCOL.md) cover loss.
    ::sendto(fd_, data, size, 0, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
}

}  // namespace sfcop::relay
