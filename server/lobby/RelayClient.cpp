#include "RelayClient.h"

#include "Log.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace sfcop::lobby {

namespace {
constexpr const char* kTag = "relay-client";

std::string hex16(uint64_t v) {
    static const char* d = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i) {
        s[size_t(i)] = d[v & 15];
        v >>= 4;
    }
    return s;
}
} // namespace

std::string relayRemovalRequest(uint64_t sessionId) {
    return "DELETE /v1/bindings/" + hex16(sessionId) + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
}

int httpStatusOf(const std::string& response) {
    if (response.size() < 12 || response.compare(0, 5, "HTTP/") != 0 || response[8] != ' ') return 0;
    int status = 0;
    for (size_t i = 9; i < 12; ++i) {
        if (response[i] < '0' || response[i] > '9') return 0;
        status = status * 10 + (response[i] - '0');
    }
    return status;
}

void HttpRelayClient::removeSession(uint64_t sessionId) {
    auto answer = transport_(relayRemovalRequest(sessionId));
    if (!answer) {
        log::debug(kTag, "the relay could not be reached; its bindings lapse by themselves");
        return;
    }
    int status = httpStatusOf(*answer);
    if (status != 204) {
        log::debug(kTag, "the relay answered " + std::to_string(status) + " to a removal");
    }
}

RelayTransport loopbackTransport(uint16_t port) {
    return [port](const std::string& request) -> std::optional<std::string> {
        using Clock = std::chrono::steady_clock;
        auto deadline = Clock::now() + std::chrono::milliseconds(100);
        auto remaining = [&] {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            return ms < 0 ? 0 : int(ms);
        };
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return std::nullopt;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
        std::optional<std::string> result;
        int rc = connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa);
        bool connected = rc == 0;
        if (!connected && errno == EINPROGRESS) {
            pollfd p{fd, POLLOUT, 0};
            if (poll(&p, 1, remaining()) == 1) {
                int err = 0;
                socklen_t len = sizeof err;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                connected = err == 0;
            }
        }
        if (connected) {
            size_t sent = 0;
            while (sent < request.size()) {
                ssize_t n = send(fd, request.data() + sent, request.size() - sent, 0);
                if (n <= 0) break;
                sent += size_t(n);
            }
            if (sent == request.size()) {
                std::string answer;
                char buf[512];
                while (true) {
                    pollfd p{fd, POLLIN, 0};
                    if (poll(&p, 1, remaining()) != 1) break;
                    ssize_t n = recv(fd, buf, sizeof buf, 0);
                    if (n <= 0) break;
                    answer.append(buf, size_t(n));
                    if (answer.find("\r\n") != std::string::npos) break; // the status line is enough
                }
                if (!answer.empty()) result = answer;
            }
        }
        close(fd);
        return result;
    };
}

} // namespace sfcop::lobby
