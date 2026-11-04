// A Transport and a Rendezvous for tests: record each call and return the values the test
// programs. Receive() and ReceiveFrom() hand out `incoming` datagrams front first (kWouldBlock
// when it is empty); Send() and SendTo() append to `sent`. Select() returns `selectReady` as the
// ready sets.
#pragma once

#include "platform/Core/MockLog.h"
#include "platform/Transport/Transport.h"

#include <algorithm>
#include <cstring>
#include <deque>

namespace sfcop::platform {

struct MockDatagram {
    std::vector<std::uint8_t> bytes;
    Endpoint peer; // the sender (incoming) or the destination (sent; zero for Send())
};

class RendezvousMock final : public Rendezvous {
public:
    MockLog log;
    Result<SocketId> connectResult = SocketId{100};

    Result<SocketId> ConnectToPeer(std::string_view sessionId, std::uint32_t timeoutMs) override {
        log.Record("ConnectToPeer", sessionId, timeoutMs);
        return connectResult;
    }
};

class TransportMock final : public Transport {
public:
    MockLog log;

    std::deque<MockDatagram> incoming;
    std::vector<MockDatagram> sent;
    SelectSet selectReady;
    RendezvousMock rendezvous;

    Result<SocketId> openResult = SocketId{1};
    Status closeResult;
    Status setOptionResult;
    Result<std::int32_t> getOptionResult = std::int32_t{0};
    Status bindResult;
    Status connectResult;
    Status listenResult;
    Result<Accepted> acceptResult = Accepted{SocketId{2}, Endpoint{}};
    Error sendError = Error::kNone; // when set, Send() and SendTo() fail with it
    Status shutdownResult;
    Result<Endpoint> peerNameResult = Endpoint{};
    Result<Endpoint> localNameResult = Endpoint{};
    Error selectError = Error::kNone;
    Result<std::vector<std::uint32_t>> resolveResult = std::vector<std::uint32_t>{};
    Result<std::string> hostNameResult = std::string("testhost");
    bool rendezvousAvailable = true;

    Result<SocketId> Open(SocketKind kind) override {
        log.Record("Open", kind);
        return openResult;
    }
    Status Close(SocketId socket) override {
        log.Record("Close", socket);
        return closeResult;
    }
    Status SetOption(SocketId socket, SocketOption option, std::int32_t value) override {
        log.Record("SetOption", socket, option, value);
        return setOptionResult;
    }
    Result<std::int32_t> GetOption(SocketId socket, SocketOption option) override {
        log.Record("GetOption", socket, option);
        return getOptionResult;
    }
    Status Bind(SocketId socket, Endpoint local) override {
        log.Record("Bind", socket, local.address, local.port);
        return bindResult;
    }
    Status Connect(SocketId socket, Endpoint peer) override {
        log.Record("Connect", socket, peer.address, peer.port);
        return connectResult;
    }
    Status Listen(SocketId socket, std::uint32_t backlog) override {
        log.Record("Listen", socket, backlog);
        return listenResult;
    }
    Result<Accepted> Accept(SocketId socket) override {
        log.Record("Accept", socket);
        return acceptResult;
    }
    Result<std::size_t> Send(SocketId socket, const void *data, std::size_t bytes) override {
        log.Record("Send", socket, bytes);
        return recordSent(data, bytes, Endpoint{});
    }
    Result<std::size_t> Receive(SocketId socket, void *out, std::size_t bytes) override {
        log.Record("Receive", socket, bytes);
        Result<Received> got = takeIncoming(out, bytes);
        if (!got.Ok()) {
            return got.Code();
        }
        return got.Value().bytes;
    }
    Result<std::size_t> SendTo(SocketId socket, const void *data, std::size_t bytes,
                               Endpoint peer) override {
        log.Record("SendTo", socket, bytes, peer.address, peer.port);
        return recordSent(data, bytes, peer);
    }
    Result<Received> ReceiveFrom(SocketId socket, void *out, std::size_t bytes) override {
        log.Record("ReceiveFrom", socket, bytes);
        return takeIncoming(out, bytes);
    }
    Status Shutdown(SocketId socket, ShutdownHow how) override {
        log.Record("Shutdown", socket, how);
        return shutdownResult;
    }
    Result<Endpoint> PeerName(SocketId socket) override {
        log.Record("PeerName", socket);
        return peerNameResult;
    }
    Result<Endpoint> LocalName(SocketId socket) override {
        log.Record("LocalName", socket);
        return localNameResult;
    }
    Result<std::size_t> Select(SelectSet &sets, std::optional<std::uint32_t> timeoutMs) override {
        if (timeoutMs) {
            log.Record("Select", sets.readable.size(), sets.writable.size(), sets.errored.size(),
                       *timeoutMs);
        } else {
            log.Record("Select", sets.readable.size(), sets.writable.size(), sets.errored.size(),
                       "forever");
        }
        if (selectError != Error::kNone) {
            return selectError;
        }
        sets = selectReady;
        return sets.readable.size() + sets.writable.size() + sets.errored.size();
    }
    Result<std::vector<std::uint32_t>> Resolve(std::string_view hostName) override {
        log.Record("Resolve", hostName);
        return resolveResult;
    }
    Result<std::string> HostName() override {
        log.Record("HostName");
        return hostNameResult;
    }
    Result<Rendezvous *> GetRendezvous() override {
        log.Record("GetRendezvous");
        if (!rendezvousAvailable) {
            return Error::kUnsupported;
        }
        return static_cast<Rendezvous *>(&rendezvous);
    }

private:
    Result<std::size_t> recordSent(const void *data, std::size_t bytes, Endpoint peer) {
        if (sendError != Error::kNone) {
            return sendError;
        }
        const auto *first = static_cast<const std::uint8_t *>(data);
        sent.push_back(MockDatagram{std::vector<std::uint8_t>(first, first + bytes), peer});
        return bytes;
    }

    Result<Received> takeIncoming(void *out, std::size_t bytes) {
        if (incoming.empty()) {
            return Error::kWouldBlock;
        }
        MockDatagram datagram = incoming.front();
        incoming.pop_front();
        std::size_t count = (std::min)(bytes, datagram.bytes.size());
        if (count > 0) {
            std::memcpy(out, datagram.bytes.data(), count);
        }
        return Received{count, datagram.peer};
    }
};

} // namespace sfcop::platform
