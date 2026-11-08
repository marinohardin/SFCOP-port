#include "Server.h"

#include "Lobby.h"
#include "Log.h"
#include "RelayClient.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fstream>
#include <map>
#include <stdexcept>
#include <vector>

namespace sfcop::lobby {

namespace {

constexpr const char* kTag = "service";
constexpr size_t kMaxConnections = 256;
constexpr uint64_t kConnectionIdleMs = 10000;

uint64_t wallMs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
}
uint64_t steadyMs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void fillRandom(uint8_t* out, size_t size) {
    static std::ifstream urandom;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!urandom.is_open()) urandom.open("/dev/urandom", std::ios::binary);
        urandom.read(reinterpret_cast<char*>(out), std::streamsize(size));
        if (urandom) return;
        urandom.close(); // reopen on a failed read instead of staying failed
        urandom.clear();
    }
    throw std::runtime_error("cannot read /dev/urandom");
}

void setNonBlocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

bool bindTo(int fd, const ListenAddress& a, std::string& error) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(a.port);
    inet_pton(AF_INET, a.host.c_str(), &sa.sin_addr);
    if (bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
        error = "cannot bind " + a.host + ":" + std::to_string(a.port) + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

// The last entry the trusted proxy appended, when it looks like an address; else empty.
std::string forwardedFor(const HttpRequest& req) {
    const std::string* h = req.header("x-forwarded-for");
    if (!h) return "";
    size_t comma = h->rfind(',');
    std::string last = h->substr(comma == std::string::npos ? 0 : comma + 1);
    while (!last.empty() && last.front() == ' ') last.erase(last.begin());
    while (!last.empty() && last.back() == ' ') last.pop_back();
    if (last.empty() || last.size() > 45) return "";
    for (char c : last) {
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '.' || c == ':';
        if (!ok) return "";
    }
    return last;
}

struct Connection {
    int fd = -1;
    bool viaProxy = false; // accepted on the public listener from loopback
    std::string peer;
    std::string in;
    std::string out;
    size_t sent = 0;
    uint64_t lastMs = 0;
    uint64_t startedMs = 0;
    bool responding = false;
};

} // namespace

struct Server::Impl {
    ServiceConfig config;
    std::unique_ptr<Lobby> lobby;
    std::unique_ptr<RelayClient> relay;
    int httpFd = -1, healthFd = -1;
    std::map<int, Connection> conns;
    uint64_t lastMinuteMs = 0;

    int listenTcp(const ListenAddress& a, std::string& error) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            error = std::string("socket: ") + std::strerror(errno);
            return -1;
        }
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (!bindTo(fd, a, error) || listen(fd, 64) != 0) {
            if (error.empty()) error = std::string("listen: ") + std::strerror(errno);
            close(fd);
            return -1;
        }
        setNonBlocking(fd);
        return fd;
    }

    void accept(int listener, bool publicListener, uint64_t nowSteady) {
        while (true) {
            sockaddr_in sa{};
            socklen_t len = sizeof sa;
            int fd = ::accept(listener, reinterpret_cast<sockaddr*>(&sa), &len);
            if (fd < 0) return;
            if (conns.size() >= kMaxConnections) {
                close(fd);
                continue;
            }
            setNonBlocking(fd);
            Connection c;
            c.fd = fd;
            char text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &sa.sin_addr, text, sizeof text);
            c.peer = text;
            c.viaProxy = publicListener && c.peer == "127.0.0.1";
            c.lastMs = c.startedMs = nowSteady;
            conns[fd] = std::move(c);
        }
    }

    void respond(Connection& c, const HttpResponse& r, const std::string& method) {
        c.out = serializeHttpResponse(r);
        c.responding = true;
        log::info(kTag, requestLogLine(method, r, steadyMs() - c.startedMs));
    }

    void readFrom(Connection& c, uint64_t nowSteady) {
        char buf[4096];
        while (true) {
            ssize_t n = recv(c.fd, buf, sizeof buf, 0);
            if (n > 0) {
                c.in.append(buf, size_t(n));
                c.lastMs = nowSteady;
                if (c.in.size() > kMaxHeaderBytes + kMaxBodyBytes + 16) break;
                continue;
            }
            if (n == 0) c.lastMs = 0; // closed by the peer: dropped below
            break;
        }
        if (c.responding) return;
        ParseResult p = parseHttpRequest(c.in);
        if (p.state == ParseState::NeedMore) return;
        HttpResponse r;
        std::string method = "(bad)";
        if (p.state == ParseState::Done) {
            method = p.request.method;
            p.request.source = c.peer;
            if (c.viaProxy) {
                std::string fwd = forwardedFor(p.request);
                if (!fwd.empty()) p.request.source = fwd;
            }
            r = lobby->handle(p.request, wallMs());
        } else {
            r.status = 400;
            r.body = R"({"error":{"code":"bad_request","message":"The request is malformed or too large."}})";
            r.pattern = "(malformed)";
        }
        respond(c, r, method);
    }

    void writeTo(Connection& c) {
        while (c.sent < c.out.size()) {
            ssize_t n = send(c.fd, c.out.data() + c.sent, c.out.size() - c.sent, 0);
            if (n <= 0) return;
            c.sent += size_t(n);
        }
    }

    void minuteReport() {
        log::info(kTag, "last minute: " + std::to_string(lobby->sessionCount()) + " sessions listed");
    }
};

Server::Server(ServiceConfig config) : impl_(std::make_unique<Impl>()) { impl_->config = std::move(config); }

Server::~Server() {
    if (!impl_) return;
    for (auto& kv : impl_->conns) close(kv.first);
    for (int fd : {impl_->httpFd, impl_->healthFd}) {
        if (fd >= 0) close(fd);
    }
}

bool Server::start(std::string& error) {
    Impl& s = *impl_;
    LobbyConfig lc;
    lc.rendezvousHost = s.config.rendezvousHost;
    lc.rendezvousPort = s.config.rendezvousPort;
    lc.ticketKey = s.config.ticketKey;
    lc.blocked = s.config.blocked;
    s.lobby = std::make_unique<Lobby>(lc, fillRandom, wallMs());

    s.relay = std::make_unique<HttpRelayClient>(loopbackTransport(s.config.relayHealthPort));
    s.lobby->setRelayClient(s.relay.get());

    s.httpFd = s.listenTcp(s.config.http, error);
    if (s.httpFd < 0) return false;
    s.healthFd = s.listenTcp(s.config.health, error);
    if (s.healthFd < 0) return false;
    return true;
}

void Server::run() {
    Impl& s = *impl_;
    log::info(kTag, "started");
    uint64_t lastSweep = steadyMs();
    s.lastMinuteMs = lastSweep;
    while (!stop_) {
        std::vector<pollfd> fds;
        fds.push_back({s.httpFd, POLLIN, 0});
        fds.push_back({s.healthFd, POLLIN, 0});
        for (auto& kv : s.conns) {
            short ev = kv.second.responding ? POLLOUT : POLLIN;
            fds.push_back({kv.first, ev, 0});
        }
        int ready = poll(fds.data(), nfds_t(fds.size()), 100);
        uint64_t now = steadyMs();
        if (ready > 0) {
            if (fds[0].revents & POLLIN) s.accept(s.httpFd, true, now);
            if (fds[1].revents & POLLIN) s.accept(s.healthFd, false, now);
            for (size_t i = 2; i < fds.size(); ++i) {
                auto it = s.conns.find(fds[i].fd);
                if (it == s.conns.end()) continue;
                Connection& c = it->second;
                if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                    if (!c.responding) s.readFrom(c, now);
                }
                if (c.responding) s.writeTo(c);
            }
        }
        for (auto it = s.conns.begin(); it != s.conns.end();) {
            Connection& c = it->second;
            bool done = c.responding && c.sent >= c.out.size();
            bool idle = now >= c.lastMs + kConnectionIdleMs || c.lastMs == 0;
            if (done || idle) {
                close(c.fd);
                it = s.conns.erase(it);
            } else {
                ++it;
            }
        }
        uint64_t wall = wallMs();
        if (now >= lastSweep + 1000) {
            lastSweep = now;
            s.lobby->sweep(wall);
        }
        if (now >= s.lastMinuteMs + 60000) {
            s.lastMinuteMs = now;
            s.minuteReport();
        }
    }
    log::info(kTag, "stopped");
}

} // namespace sfcop::lobby
