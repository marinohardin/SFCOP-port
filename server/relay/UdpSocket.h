// The real UDP socket behind RelayService's Sender seam (POSIX; the service runs on Linux and
// is developed on macOS).
#pragma once

#include <cstdint>
#include <string>

#include "RelayService.h"

namespace sfcop::relay {

class UdpSocket : public Sender {
public:
    UdpSocket() = default;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    ~UdpSocket();

    bool open(const std::string& address, std::uint16_t port, std::string& error);
    // Waits up to timeoutMs for one datagram. Returns its size, 0 on timeout, -1 on error.
    // Datagrams longer than `capacity` are cut to it (the service refuses anything over 1200).
    int receive(Endpoint& from, std::uint8_t* buffer, std::size_t capacity, int timeoutMs);
    void send(const Endpoint& to, const std::uint8_t* data, std::size_t size) override;
    std::uint16_t localPort() const { return localPort_; }

private:
    int fd_ = -1;
    std::uint16_t localPort_ = 0;
};

}  // namespace sfcop::relay
