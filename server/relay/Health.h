// The service's HTTP side, on the loopback only (RENDEZVOUS.md, "The service"):
//   GET    /v1/health               the lobby's `Health` object (LOBBY_API.md); sessions counts bindings
//   DELETE /v1/bindings/<session>   the lobby tells the relay a session was removed (204); the
//                                   relay's loop drops the session's bindings on its next turn
// The request handler is a pure function so a test needs no socket.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sfcop::relay {

class AdminState {
public:
    std::atomic<std::uint64_t> bindings{0};
    std::atomic<std::uint64_t> uptimeSeconds{0};

    void queueRemoval(std::uint64_t sessionId);
    std::vector<std::uint64_t> takeRemovals();

private:
    std::mutex mutex_;
    std::vector<std::uint64_t> removals_;
};

// `request` is the raw bytes read from the connection; the answer is the complete response.
std::string handleHttpRequest(const std::string& request, AdminState& state);

class HttpListener {
public:
    HttpListener() = default;
    HttpListener(const HttpListener&) = delete;
    HttpListener& operator=(const HttpListener&) = delete;
    ~HttpListener() { stop(); }

    // `address` is the dotted quad to listen on (127.0.0.1 in production). False on failure.
    bool start(const std::string& address, std::uint16_t port, AdminState& state);
    void stop();

private:
    int fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace sfcop::relay
