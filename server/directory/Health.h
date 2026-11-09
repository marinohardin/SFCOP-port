// The service's loopback HTTP side, for the host's monitor (DESIGN §10):
//   GET /v1/health   200 with {"status":"ok","api":1,"service":"directory","entries":N,"uptime_s":S}
// The request handler is a pure function so a test needs no socket.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace sfcop::directory {

struct HealthState {
    std::atomic<std::uint64_t> entries{0};       // campaign servers listed
    std::atomic<std::uint64_t> uptimeSeconds{0};
};

// `request` is the raw bytes read from the connection; the answer is the complete response.
std::string handleHealthRequest(const std::string& request, const HealthState& state);

class HealthListener {
public:
    HealthListener() = default;
    HealthListener(const HealthListener&) = delete;
    HealthListener& operator=(const HealthListener&) = delete;
    ~HealthListener() { stop(); }

    // `address` is the dotted quad to listen on (127.0.0.1 in production). False on failure.
    bool start(const std::string& address, std::uint16_t port, const HealthState& state);
    void stop();

private:
    int fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace sfcop::directory
