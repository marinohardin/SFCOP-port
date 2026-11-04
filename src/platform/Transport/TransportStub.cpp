// The stub backend: compiles on every platform and supports nothing.
#include "platform/Transport/Transport.h"

namespace sfcop::platform {
namespace {

class TransportStub final : public Transport {
public:
    Result<SocketId> Open(SocketKind) override { return Error::kUnsupported; }
    Status Close(SocketId) override { return Error::kUnsupported; }
    Status SetOption(SocketId, SocketOption, std::int32_t) override {
        return Error::kUnsupported;
    }
    Result<std::int32_t> GetOption(SocketId, SocketOption) override {
        return Error::kUnsupported;
    }
    Status Bind(SocketId, Endpoint) override { return Error::kUnsupported; }
    Status Connect(SocketId, Endpoint) override { return Error::kUnsupported; }
    Status Listen(SocketId, std::uint32_t) override { return Error::kUnsupported; }
    Result<Accepted> Accept(SocketId) override { return Error::kUnsupported; }
    Result<std::size_t> Send(SocketId, const void *, std::size_t) override {
        return Error::kUnsupported;
    }
    Result<std::size_t> Receive(SocketId, void *, std::size_t) override {
        return Error::kUnsupported;
    }
    Result<std::size_t> SendTo(SocketId, const void *, std::size_t, Endpoint) override {
        return Error::kUnsupported;
    }
    Result<Received> ReceiveFrom(SocketId, void *, std::size_t) override {
        return Error::kUnsupported;
    }
    Status Shutdown(SocketId, ShutdownHow) override { return Error::kUnsupported; }
    Result<Endpoint> PeerName(SocketId) override { return Error::kUnsupported; }
    Result<Endpoint> LocalName(SocketId) override { return Error::kUnsupported; }
    Result<std::size_t> Select(SelectSet &, std::optional<std::uint32_t>) override {
        return Error::kUnsupported;
    }
    Result<std::vector<std::uint32_t>> Resolve(std::string_view) override {
        return Error::kUnsupported;
    }
    Result<std::string> HostName() override { return Error::kUnsupported; }
    Result<Rendezvous *> GetRendezvous() override { return Error::kUnsupported; }
};

} // namespace

std::unique_ptr<Transport> makeTransportStub() { return std::make_unique<TransportStub>(); }

} // namespace sfcop::platform
