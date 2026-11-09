// A small poll() loop for TCP services: one listening socket, many connections, each driven by a
// StreamSession that turns the bytes it is given into bytes to send. The protocol code lives in
// the sessions, which are plain objects a test drives without a socket.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sfcop::auth {

class StreamSession {
public:
    virtual ~StreamSession() = default;
    // Bytes the peer sent, in order; a session keeps what it cannot use yet.
    virtual void onBytes(const std::uint8_t* data, std::size_t size) = 0;
    // What to send now; the loop sends it all before reading again.
    std::string takeOutput() {
        std::string out;
        out.swap(output_);
        return out;
    }
    // True once the session wants the connection closed after its output is sent.
    bool closing() const { return closing_; }

protected:
    void send(const std::string& bytes) { output_ += bytes; }
    void close() { closing_ = true; }

private:
    std::string output_;
    bool closing_ = false;
};

struct TcpServerLimits {
    std::size_t maxConnections = 256;
    std::uint32_t idleMs = 60000;      // a connection that sends nothing for this long is closed
    std::size_t maxPendingOutput = 1 << 20;
};

class TcpServer {
public:
    // `remoteAddress` is the peer's IPv4 address in host order; the factory may return null to
    // refuse a connection.
    using Factory = std::function<std::unique_ptr<StreamSession>(std::uint32_t remoteAddress)>;

    TcpServer(Factory factory, TcpServerLimits limits = TcpServerLimits());
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    ~TcpServer();

    // Listens on a dotted quad and port (0 picks a free port). False with `error` on failure.
    bool listen(const std::string& address, std::uint16_t port, std::string& error);
    std::uint16_t port() const { return port_; }

    // Waits at most `timeoutMs` for something to do, then does it. `nowMs` is a monotonic clock.
    void poll(std::uint32_t timeoutMs, std::uint64_t nowMs);

    std::size_t connectionCount() const { return connections_.size(); }

private:
    struct Connection {
        int fd = -1;
        std::unique_ptr<StreamSession> session;
        std::string pending;
        std::uint64_t lastActivityMs = 0;
        bool dead = false;
    };
    void accept(std::uint64_t nowMs);
    void flush(Connection& c);

    Factory factory_;
    TcpServerLimits limits_;
    int listenFd_ = -1;
    std::uint16_t port_ = 0;
    std::vector<Connection> connections_;
};

}  // namespace sfcop::auth
