#include "net/RendezvousTransport.h"

#include "net/PeerNames.h"
#include "net/RendezvousEngine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace sfcop::net {
namespace {

using platform::Endpoint;
using platform::Error;
using platform::Result;
using platform::SelectSet;
using platform::SocketId;
using platform::SocketOption;
using platform::Status;

constexpr std::uint32_t kVirtualBit = 0x80000000u;
constexpr std::uint32_t kPumpMs = 10;
constexpr std::size_t kMaxDatagram = 65507;

bool isVirtual(SocketId id) {
    return (id.value & kVirtualBit) != 0;
}

// One socket handed out by ConnectToPeer() or AcceptPeer().
struct PeerSocket {
    std::uint32_t peer = 0;
    bool nonBlocking = false;
    bool broadcast = false;
    bool reuseAddress = false;
    std::int32_t receiveBuffer = 65536;
    std::int32_t sendBuffer = 65536;
};

class RendezvousTransport final : public platform::Transport, public platform::Rendezvous {
public:
    RendezvousTransport(std::unique_ptr<platform::Transport> inner, RendezvousConfig config)
        : inner_(std::move(inner)), config_(std::move(config)),
          epoch_(std::chrono::steady_clock::now()) {}

    ~RendezvousTransport() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        changed_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
        engine_.reset();
    }

    // Transport: the backend's sockets pass through; the peer sockets are served here.

    Result<SocketId> Open(platform::SocketKind kind) override {
        Result<SocketId> opened = inner_->Open(kind);
        if (opened.Ok() && isVirtual(opened.Value())) {
            (void)inner_->Close(opened.Value());
            return Error::kOutOfResources;
        }
        return opened;
    }

    Status Close(SocketId id) override {
        if (!isVirtual(id)) {
            return inner_->Close(id);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = sockets_.find(id.value);
        if (found == sockets_.end()) {
            return Error::kInvalidHandle;
        }
        engine_->Release(found->second.peer, now());
        sockets_.erase(found);
        changed_.notify_all();
        return {};
    }

    Status SetOption(SocketId id, SocketOption option, std::int32_t value) override {
        if (!isVirtual(id)) {
            return inner_->SetOption(id, option, value);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        PeerSocket *socket = find(id);
        if (!socket) {
            return Error::kInvalidHandle;
        }
        switch (option) {
        case SocketOption::kNonBlocking:
        case SocketOption::kBroadcast:
        case SocketOption::kReuseAddress:
            if (value != 0 && value != 1) {
                return Error::kInvalidArgument;
            }
            (option == SocketOption::kNonBlocking ? socket->nonBlocking
             : option == SocketOption::kBroadcast ? socket->broadcast
                                                  : socket->reuseAddress) = value != 0;
            return {};
        case SocketOption::kReceiveBuffer:
        case SocketOption::kSendBuffer:
            if (value < 0) {
                return Error::kInvalidArgument;
            }
            (option == SocketOption::kReceiveBuffer ? socket->receiveBuffer : socket->sendBuffer) =
                value;
            return {};
        case SocketOption::kNoDelay:
        case SocketOption::kKeepAlive:
        case SocketOption::kLingerSeconds:
        case SocketOption::kBytesReadable:
            break;
        }
        return Error::kInvalidArgument;
    }

    Result<std::int32_t> GetOption(SocketId id, SocketOption option) override {
        if (!isVirtual(id)) {
            return inner_->GetOption(id, option);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        PeerSocket *socket = find(id);
        if (!socket) {
            return Error::kInvalidHandle;
        }
        switch (option) {
        case SocketOption::kNonBlocking:
            return std::int32_t{socket->nonBlocking ? 1 : 0};
        case SocketOption::kBroadcast:
            return std::int32_t{socket->broadcast ? 1 : 0};
        case SocketOption::kReuseAddress:
            return std::int32_t{socket->reuseAddress ? 1 : 0};
        case SocketOption::kReceiveBuffer:
            return socket->receiveBuffer;
        case SocketOption::kSendBuffer:
            return socket->sendBuffer;
        case SocketOption::kBytesReadable:
            return static_cast<std::int32_t>(engine_->NextMessageBytes(socket->peer));
        case SocketOption::kNoDelay:
        case SocketOption::kKeepAlive:
        case SocketOption::kLingerSeconds:
            break;
        }
        return Error::kInvalidArgument;
    }

    Status Bind(SocketId id, Endpoint local) override {
        if (!isVirtual(id)) {
            return inner_->Bind(id, local);
        }
        return peerSocketOnly(id, Error::kWrongState); // bound already, to the binding's port
    }

    Status Connect(SocketId id, Endpoint peer) override {
        if (!isVirtual(id)) {
            return inner_->Connect(id, peer);
        }
        return peerSocketOnly(id, Error::kWrongState); // its one peer is fixed
    }

    Status Listen(SocketId id, std::uint32_t backlog) override {
        if (!isVirtual(id)) {
            return inner_->Listen(id, backlog);
        }
        return peerSocketOnly(id, Error::kWrongState);
    }

    Result<platform::Accepted> Accept(SocketId id) override {
        if (!isVirtual(id)) {
            return inner_->Accept(id);
        }
        Status known = peerSocketOnly(id, Error::kWrongState);
        return known.Code();
    }

    Result<std::size_t> Send(SocketId id, const void *data, std::size_t bytes) override {
        if (!isVirtual(id)) {
            return inner_->Send(id, data, bytes);
        }
        return sendToPeer(id, data, bytes);
    }

    Result<std::size_t> Receive(SocketId id, void *out, std::size_t bytes) override {
        if (!isVirtual(id)) {
            return inner_->Receive(id, out, bytes);
        }
        Result<platform::Received> got = receiveFromPeer(id, out, bytes);
        if (!got.Ok()) {
            return got.Code();
        }
        return got.Value().bytes;
    }

    Result<std::size_t> SendTo(SocketId id, const void *data, std::size_t bytes,
                               Endpoint peer) override {
        if (!isVirtual(id)) {
            return inner_->SendTo(id, data, bytes, peer);
        }
        return sendToPeer(id, data, bytes); // a peer socket has one destination
    }

    Result<platform::Received> ReceiveFrom(SocketId id, void *out, std::size_t bytes) override {
        if (!isVirtual(id)) {
            return inner_->ReceiveFrom(id, out, bytes);
        }
        return receiveFromPeer(id, out, bytes);
    }

    Status Shutdown(SocketId id, platform::ShutdownHow how) override {
        if (!isVirtual(id)) {
            return inner_->Shutdown(id, how);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return find(id) ? Status{} : Status{Error::kInvalidHandle};
    }

    Result<Endpoint> PeerName(SocketId id) override {
        if (!isVirtual(id)) {
            return inner_->PeerName(id);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        PeerSocket *socket = find(id);
        if (!socket) {
            return Error::kInvalidHandle;
        }
        return engine_->PathEndpoint(socket->peer);
    }

    Result<Endpoint> LocalName(SocketId id) override {
        if (!isVirtual(id)) {
            return inner_->LocalName(id);
        }
        SocketId shared;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!find(id)) {
                return Error::kInvalidHandle;
            }
            shared = engine_->Socket();
        }
        return inner_->LocalName(shared);
    }

    Result<std::size_t> Select(SelectSet &sets, std::optional<std::uint32_t> timeoutMs) override;

    Result<std::vector<std::uint32_t>> Resolve(std::string_view hostName) override {
        return inner_->Resolve(hostName);
    }

    Result<std::string> HostName() override { return inner_->HostName(); }

    Result<platform::Rendezvous *> GetRendezvous() override {
        if (config_.serviceHost.empty()) {
            return Error::kUnsupported;
        }
        return static_cast<platform::Rendezvous *>(this);
    }

    std::uint64_t Pumps() const { return pumps_.load(); }

    // Rendezvous.

    Status Bind(std::string_view sessionId, std::uint32_t peerId, const std::uint8_t *ticket,
                std::size_t ticketBytes, std::uint32_t timeoutMs) override;
    Status Unbind() override;
    Result<SocketId> ConnectToPeer(std::string_view peerName, std::uint32_t timeoutMs) override;
    Result<SocketId> AcceptPeer(std::uint32_t timeoutMs) override;

private:
    std::unique_ptr<platform::Transport> inner_;
    RendezvousConfig config_;
    std::chrono::steady_clock::time_point epoch_;

    std::mutex mutex_;
    std::condition_variable changed_; // notified after every pump and every close
    std::unique_ptr<RendezvousEngine> engine_;
    std::thread thread_;
    std::atomic<std::uint64_t> pumps_{0};
    bool stop_ = false;
    std::map<std::uint32_t, PeerSocket> sockets_;
    std::uint32_t nextSocket_ = kVirtualBit | 1;

    std::uint32_t now() const {
        return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now() - epoch_)
                                              .count());
    }

    static std::chrono::steady_clock::time_point deadline(std::uint32_t timeoutMs) {
        return std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    }

    PeerSocket *find(SocketId id) {
        auto found = sockets_.find(id.value);
        return found == sockets_.end() ? nullptr : &found->second;
    }

    Status peerSocketOnly(SocketId id, Error error) {
        std::lock_guard<std::mutex> lock(mutex_);
        return find(id) ? Status{error} : Status{Error::kInvalidHandle};
    }

    bool bound() const {
        return engine_ && engine_->Binding() == RendezvousEngine::BindState::kBound;
    }

    SocketId handOut(std::uint32_t peer) {
        while (sockets_.count(nextSocket_) != 0 || nextSocket_ == kVirtualBit) {
            nextSocket_ = kVirtualBit | (nextSocket_ + 1);
        }
        SocketId id{nextSocket_};
        nextSocket_ = kVirtualBit | (nextSocket_ + 1);
        sockets_[id.value] = PeerSocket{peer};
        changed_.notify_all();
        return id;
    }

    Result<std::size_t> sendToPeer(SocketId id, const void *data, std::size_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        PeerSocket *socket = find(id);
        if (!socket) {
            return Error::kInvalidHandle;
        }
        if (bytes > kMaxDatagram) {
            return Error::kInvalidArgument;
        }
        if (bytes == 0) {
            return std::size_t{0}; // an empty datagram carries no session message
        }
        Status sent =
            engine_->Send(socket->peer, static_cast<const std::uint8_t *>(data), bytes, now());
        if (!sent.Ok()) {
            return sent.Code();
        }
        return bytes;
    }

    Result<platform::Received> receiveFromPeer(SocketId id, void *out, std::size_t bytes) {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            PeerSocket *socket = find(id);
            if (!socket) {
                return Error::kNotConnected; // closed, perhaps by another thread while waiting
            }
            const std::uint32_t peer = socket->peer;
            if (engine_->Readable(peer)) {
                std::vector<std::uint8_t> message = engine_->TakeMessage(peer);
                std::size_t count = std::min(bytes, message.size());
                if (count > 0) {
                    std::memcpy(out, message.data(), count);
                }
                return platform::Received{count, engine_->PathEndpoint(peer)};
            }
            if (engine_->Gone(peer, now())) {
                return Error::kConnectionReset;
            }
            if (socket->nonBlocking) {
                return Error::kWouldBlock;
            }
            changed_.wait_for(lock, std::chrono::milliseconds(50));
        }
    }

    void startThread() {
        if (thread_.joinable()) {
            return;
        }
        thread_ = std::thread([this] { run(); });
    }

    // Nothing to pump for: no binding held or being made, no peer, no socket handed out.
    bool idle() const { return engine_->Idle() && sockets_.empty(); }

    void run() {
        for (;;) {
            SocketId shared;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // Parked while idle; Bind() and handOut() wake it.
                changed_.wait(lock, [&] { return stop_ || !idle(); });
                if (stop_) {
                    return;
                }
                engine_->Pump(now());
                ++pumps_;
                shared = engine_->Socket();
            }
            changed_.notify_all();
            SelectSet wait;
            wait.readable.push_back(shared);
            Result<std::size_t> ready = inner_->Select(wait, kPumpMs);
            if (!ready.Ok()) {
                std::unique_lock<std::mutex> lock(mutex_);
                changed_.wait_for(lock, std::chrono::milliseconds(kPumpMs));
            }
        }
    }

    Result<Endpoint> resolveService() {
        Result<std::vector<std::uint32_t>> addresses = inner_->Resolve(config_.serviceHost);
        if (!addresses.Ok() || addresses.Value().empty()) {
            return Error::kTimedOut; // the service cannot be reached: it did not answer
        }
        return Endpoint{addresses.Value().front(), config_.servicePort};
    }
};

Result<std::size_t> RendezvousTransport::Select(SelectSet &sets,
                                                std::optional<std::uint32_t> timeoutMs) {
    auto anyVirtual = [](const std::vector<SocketId> &list) {
        return std::any_of(list.begin(), list.end(), isVirtual);
    };
    if (!anyVirtual(sets.readable) && !anyVirtual(sets.writable) && !anyVirtual(sets.errored)) {
        return inner_->Select(sets, timeoutMs);
    }
    SelectSet real;
    auto realOnly = [](const std::vector<SocketId> &list, std::vector<SocketId> &out) {
        for (SocketId id : list) {
            if (!isVirtual(id)) {
                out.push_back(id);
            }
        }
    };
    realOnly(sets.readable, real.readable);
    realOnly(sets.writable, real.writable);
    realOnly(sets.errored, real.errored);
    const bool haveReal = !real.readable.empty() || !real.writable.empty() || !real.errored.empty();
    const auto until = timeoutMs ? std::optional(deadline(*timeoutMs)) : std::nullopt;

    std::unique_lock<std::mutex> lock(mutex_);
    for (const std::vector<SocketId> *list : {&sets.readable, &sets.writable, &sets.errored}) {
        for (SocketId id : *list) {
            if (isVirtual(id) && !find(id)) {
                return Error::kInvalidHandle;
            }
        }
    }
    for (;;) {
        // A peer socket is readable with a message waiting or once its path is gone (the next
        // Receive() reports it), always writable, and never errored.
        std::size_t virtualReady = 0;
        auto readable = [&](SocketId id) {
            PeerSocket *socket = find(id);
            return !socket || engine_->Readable(socket->peer) || engine_->Gone(socket->peer, now());
        };
        for (SocketId id : sets.readable) {
            virtualReady += isVirtual(id) && readable(id) ? 1 : 0;
        }
        for (SocketId id : sets.writable) {
            virtualReady += isVirtual(id) ? 1 : 0;
        }
        SelectSet realReady = real;
        std::size_t realCount = 0;
        if (haveReal) {
            std::uint32_t slice = virtualReady > 0 ? 0 : kPumpMs;
            if (until) {
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    *until - std::chrono::steady_clock::now());
                slice = std::min<std::uint32_t>(
                    slice, static_cast<std::uint32_t>(std::max<long long>(0, left.count())));
            }
            lock.unlock();
            Result<std::size_t> got = inner_->Select(realReady, slice);
            lock.lock();
            if (!got.Ok()) {
                return got.Code();
            }
            realCount = got.Value();
        }
        const bool timedOut = until && std::chrono::steady_clock::now() >= *until;
        if (virtualReady + realCount > 0 || timedOut) {
            auto keep = [&](std::vector<SocketId> &list, const std::vector<SocketId> &readyReal,
                            bool virtualKept) {
                std::vector<SocketId> kept;
                for (SocketId id : list) {
                    bool ready =
                        isVirtual(id)
                            ? virtualKept && (&list != &sets.readable || readable(id))
                            : std::find(readyReal.begin(), readyReal.end(), id) != readyReal.end();
                    if (ready) {
                        kept.push_back(id);
                    }
                }
                list = std::move(kept);
            };
            const bool any = virtualReady + realCount > 0;
            keep(sets.readable, any ? realReady.readable : std::vector<SocketId>{}, any);
            keep(sets.writable, any ? realReady.writable : std::vector<SocketId>{}, any);
            keep(sets.errored, any ? realReady.errored : std::vector<SocketId>{}, false);
            return sets.readable.size() + sets.writable.size() + sets.errored.size();
        }
        if (!haveReal) {
            auto wait = std::chrono::milliseconds(kPumpMs);
            if (until) {
                changed_.wait_until(lock,
                                    std::min(*until, std::chrono::steady_clock::now() + wait));
            } else {
                changed_.wait_for(lock, wait);
            }
        }
    }
}

Status RendezvousTransport::Bind(std::string_view sessionId, std::uint32_t peerId,
                                 const std::uint8_t *ticket, std::size_t ticketBytes,
                                 std::uint32_t timeoutMs) {
    if (config_.serviceHost.empty()) {
        return Error::kUnsupported;
    }
    std::optional<std::uint64_t> session = parseSessionId(sessionId);
    if (!session || peerId == 0 || !ticket || ticketBytes != kTicketBytes) {
        return Error::kInvalidArgument;
    }
    Ticket copy{};
    std::memcpy(copy.data(), ticket, kTicketBytes);
    const auto until = deadline(timeoutMs);

    // The lookups run before the lock is taken: Resolve() may block for seconds (Transport.h),
    // and the service thread and every peer socket wait on mutex_. Their time counts against
    // timeoutMs, though a lookup already started cannot be cut short (Resolve() takes no
    // timeout). The local candidates are looked up once per Bind() and reused by its refreshes.
    std::vector<std::uint32_t> local;
    if (config_.localCandidates) {
        local = lookUpLocalAddresses(*inner_);
    }
    bool haveEngine = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        haveEngine = engine_ != nullptr;
    }
    std::optional<Endpoint> service;
    if (!haveEngine) {
        Result<Endpoint> resolved = resolveService();
        if (!resolved.Ok()) {
            return resolved.Code();
        }
        service = resolved.Value();
    }
    if (std::chrono::steady_clock::now() >= until) {
        return Error::kTimedOut;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (!engine_) {
        if (!service) {
            return Error::kTimedOut; // not reached: the engine, once made, lives as long as this
        }
        RendezvousSettings settings;
        settings.service = *service;
        settings.forceRelay = config_.forceRelay;
        settings.localAddresses = std::move(local);
        settings.seed =
            static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        engine_ = std::make_unique<RendezvousEngine>(*inner_, std::move(settings));
    } else {
        engine_->SetLocalAddresses(std::move(local));
    }
    Status started = engine_->StartBind(*session, peerId, copy, now());
    if (!started.Ok()) {
        return started;
    }
    startThread();
    changed_.notify_all(); // wakes the service thread if it was parked
    bool answered = changed_.wait_until(
        lock, until, [&] { return engine_->Binding() != RendezvousEngine::BindState::kBinding; });
    if (!answered) {
        engine_->AbandonBind(now());
        return Error::kTimedOut;
    }
    if (engine_->Binding() == RendezvousEngine::BindState::kBound) {
        return {};
    }
    return engine_->BindError();
}

Status RendezvousTransport::Unbind() {
    if (config_.serviceHost.empty()) {
        return Error::kUnsupported;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!engine_) {
        return Error::kWrongState;
    }
    return engine_->Unbind(now());
}

Result<SocketId> RendezvousTransport::ConnectToPeer(std::string_view peerName,
                                                    std::uint32_t timeoutMs) {
    if (config_.serviceHost.empty()) {
        return Error::kUnsupported;
    }
    std::optional<net::PeerName> name = parsePeerName(peerName);
    if (!name) {
        return Error::kInvalidArgument;
    }
    const auto until = deadline(timeoutMs);
    std::unique_lock<std::mutex> lock(mutex_);
    if (!bound()) {
        return Error::kWrongState;
    }
    if (name->session != engine_->Session()) {
        return Error::kInvalidArgument;
    }
    Result<RendezvousEngine::OpId> op = engine_->StartConnect(name->peer, now());
    if (!op.Ok()) {
        return op.Code();
    }
    const RendezvousEngine::OpId id = op.Value();
    bool finished = changed_.wait_until(lock, until, [&] { return engine_->Op(id).done || stop_; });
    if (!finished || stop_) {
        engine_->CancelOp(id, now());
        return Error::kTimedOut;
    }
    RendezvousEngine::OpStatus status = engine_->Op(id);
    engine_->FinishOp(id);
    if (status.error != Error::kNone) {
        return status.error;
    }
    return handOut(status.peer);
}

Result<SocketId> RendezvousTransport::AcceptPeer(std::uint32_t timeoutMs) {
    if (config_.serviceHost.empty()) {
        return Error::kUnsupported;
    }
    const auto until = deadline(timeoutMs);
    std::unique_lock<std::mutex> lock(mutex_);
    if (!bound()) {
        return Error::kWrongState;
    }
    const std::uint32_t failures = engine_->AcceptFailures();
    std::optional<std::uint32_t> peer;
    bool arrived = changed_.wait_until(lock, until, [&] {
        peer = engine_->TakeAccepted();
        return peer.has_value() || engine_->AcceptFailures() != failures || stop_;
    });
    if (peer) {
        return handOut(*peer);
    }
    if (arrived && !stop_) {
        return Error::kHostUnreachable;
    }
    return Error::kTimedOut;
}

} // namespace

bool parseServiceAddress(const std::string &text, RendezvousConfig &config) {
    std::string host = text;
    std::uint16_t port = 47710;
    std::size_t colon = text.rfind(':');
    if (colon != std::string::npos) {
        host = text.substr(0, colon);
        std::string digits = text.substr(colon + 1);
        if (digits.empty() || digits.size() > 5 ||
            !std::all_of(digits.begin(), digits.end(),
                         [](char c) { return c >= '0' && c <= '9'; })) {
            return false;
        }
        unsigned long value = std::strtoul(digits.c_str(), nullptr, 10);
        if (value == 0 || value > 65535) {
            return false;
        }
        port = static_cast<std::uint16_t>(value);
    }
    if (host.empty()) {
        return false;
    }
    config.serviceHost = host;
    config.servicePort = port;
    return true;
}

std::uint64_t rendezvousPumps(platform::Transport &transport) {
    auto *wrapper = dynamic_cast<RendezvousTransport *>(&transport);
    return wrapper ? wrapper->Pumps() : 0;
}

RendezvousConfig rendezvousConfigFromEnvironment() {
    RendezvousConfig config;
    if (const char *address = std::getenv("SFCOP_RENDEZVOUS")) {
        (void)parseServiceAddress(address, config);
    }
    if (const char *force = std::getenv("SFCOP_RENDEZVOUS_FORCE_RELAY")) {
        config.forceRelay = std::strcmp(force, "1") == 0;
    }
    return config;
}

std::unique_ptr<platform::Transport>
makeRendezvousTransport(std::unique_ptr<platform::Transport> inner, RendezvousConfig config) {
    return std::make_unique<RendezvousTransport>(std::move(inner), std::move(config));
}

} // namespace sfcop::net
