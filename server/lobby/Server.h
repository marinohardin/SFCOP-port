// The sockets around Lobby: two TCP listeners for the HTTP API (the public one behind the
// host's TLS proxy, and a loopback one for the host's monitor) and one poll loop. POSIX only; the
// service runs on Linux and the Mac runs it for the container test. Not unit tested: the handlers and the state machine, which hold the logic,
// are; the deploy test drives this layer with real requests.
#pragma once

#include "Config.h"

#include <atomic>
#include <memory>
#include <string>

namespace sfcop::lobby {

class Server {
public:
    explicit Server(ServiceConfig config);
    ~Server();

    // Binds the three sockets; false with `error` set when one cannot be.
    bool start(std::string& error);
    // Runs until requestStop() (safe from a signal handler).
    void run();
    void requestStop() { stop_ = true; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> stop_{false};
};

} // namespace sfcop::lobby
