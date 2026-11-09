#include "Health.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace sfcop::auth {
namespace {

std::string response(int code, const char* reason, const std::string& body = std::string()) {
    std::string r = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n";
    if (!body.empty()) r += "Content-Type: application/json\r\n";
    r += "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    return r;
}

}  // namespace

std::string handleHealthRequest(const std::string& request, const HealthState& state) {
    std::size_t eol = request.find("\r\n");
    if (eol == std::string::npos) return response(400, "Bad Request");
    std::string line = request.substr(0, eol);
    std::size_t sp1 = line.find(' ');
    std::size_t sp2 = sp1 == std::string::npos ? sp1 : line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return response(400, "Bad Request");
    std::string method = line.substr(0, sp1);
    std::string path = line.substr(sp1 + 1, sp2 - sp1 - 1);
    if (path != "/v1/health") return response(404, "Not Found");
    if (method != "GET") return response(405, "Method Not Allowed");
    std::string body = "{\"status\":\"ok\",\"api\":1,\"service\":\"auth\",\"accounts\":" +
                       std::to_string(state.accounts.load()) +
                       ",\"uptime_s\":" + std::to_string(state.uptimeSeconds.load()) + "}";
    return response(200, "OK", body);
}

bool HealthListener::start(const std::string& address, std::uint16_t port, const HealthState& state) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &sa.sin_addr) != 1 ||
        ::bind(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0 || ::listen(fd_, 8) != 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    running_ = true;
    thread_ = std::thread([this, &state] {
        while (running_) {
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, 200) <= 0) continue;
            int c = ::accept(fd_, nullptr, nullptr);
            if (c < 0) continue;
            // Only the host's monitor connects here; a client that sends nothing is cut off
            // after a second.
            std::string request;
            char buf[1024];
            for (int i = 0; i < 4 && request.find("\r\n\r\n") == std::string::npos &&
                            request.size() < 4096; ++i) {
                pollfd rp{c, POLLIN, 0};
                if (::poll(&rp, 1, 250) <= 0) break;
                ssize_t n = ::recv(c, buf, sizeof buf, 0);
                if (n <= 0) break;
                request.append(buf, std::size_t(n));
            }
            std::string answer = handleHealthRequest(request, state);
            ::send(c, answer.data(), answer.size(), 0);
            ::close(c);
        }
    });
    return true;
}

void HealthListener::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace sfcop::auth
