#include "hle/winsock/Winsock.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

namespace sfcop::hle::winsock {

using platform::Endpoint;
using platform::Error;
using platform::Result;
using platform::SelectSet;
using platform::SocketId;
using platform::SocketKind;
using platform::SocketOption;
using platform::Status;

namespace {

thread_local std::int32_t tLastError = 0;

struct ErrorMapping {
    Error error;
    std::int32_t wsa;
};

// Transport errors to Winsock errors, the whole default mapping. Calls that mean something
// else by an error say so where they make it.
constexpr ErrorMapping kErrorTable[] = {
    {Error::kNone, 0},
    {Error::kUnsupported, kWsaEOpNotSupp},
    {Error::kInvalidArgument, kWsaEInval},
    {Error::kInvalidHandle, kWsaENotSock},
    {Error::kWrongState, kWsaEInval},
    {Error::kNotFound, kWsaHostNotFound},
    {Error::kAlreadyExists, kWsaEAddrInUse},
    {Error::kAccessDenied, kWsaEAcces},
    {Error::kNotOwner, kWsaEInval},
    {Error::kOutOfResources, kWsaENoBufs},
    {Error::kIoError, kWsaENetDown},
    {Error::kWouldBlock, kWsaEWouldBlock},
    {Error::kInProgress, kWsaEWouldBlock}, // Winsock 1.1: a non-blocking connect
    {Error::kTimedOut, kWsaETimedOut},
    {Error::kConnectionRefused, kWsaEConnRefused},
    {Error::kConnectionReset, kWsaEConnReset},
    {Error::kNotConnected, kWsaENotConn},
    {Error::kAddressInUse, kWsaEAddrInUse},
    {Error::kHostUnreachable, kWsaEHostUnreach},
};

std::int32_t fail(std::int32_t error) {
    tLastError = error;
    return kSocketError;
}

std::uint32_t failSocket(std::int32_t error) {
    tLastError = error;
    return kInvalidSocket;
}

Endpoint toEndpoint(const SockAddrIn &address) { return Endpoint{address.address, address.port}; }

SockAddrIn toSockAddr(Endpoint endpoint) {
    SockAddrIn out{};
    out.family = kAfInet;
    out.port = endpoint.port;
    out.address = endpoint.address;
    return out;
}

// Checks an out-parameter pair (address, length) as Winsock does: an address needs a length of
// at least 16.
bool outAddressFits(const SockAddrIn *address, const std::int32_t *length) {
    return address == nullptr || (length != nullptr && *length >= kSockAddrInSize);
}

void writeAddress(SockAddrIn *address, std::int32_t *length, Endpoint endpoint) {
    if (address) {
        *address = toSockAddr(endpoint);
    }
    if (length) {
        *length = kSockAddrInSize;
    }
}

// One part of an inet_addr string; returns the text after it, or nullptr if malformed.
const char *parsePart(const char *text, std::uint32_t &value) {
    int base = 10;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text += 2;
    } else if (text[0] == '0' && text[1] >= '0' && text[1] <= '9') {
        base = 8;
        ++text;
    }
    std::uint64_t total = 0;
    const char *start = text;
    for (;; ++text) {
        int digit = -1;
        char c = *text;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        }
        if (digit < 0 || digit >= base) {
            break;
        }
        total = total * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(digit);
        if (total > 0xFFFFFFFFu) {
            return nullptr;
        }
    }
    if (text == start && base != 8) {
        return nullptr; // no digits ("0" alone is the octal branch's zero)
    }
    value = static_cast<std::uint32_t>(total);
    return text;
}

} // namespace

std::int32_t wsaErrorOf(Error error) {
    for (const ErrorMapping &row : kErrorTable) {
        if (row.error == error) {
            return row.wsa;
        }
    }
    return kWsaENetDown;
}

std::uint32_t inetAddr(const char *text) {
    constexpr std::uint32_t kNone = 0xFFFFFFFF;
    if (!text || *text == '\0') {
        return kNone;
    }
    std::uint32_t parts[4] = {};
    int count = 0;
    const char *at = text;
    for (;;) {
        if (count == 4) {
            return kNone;
        }
        at = parsePart(at, parts[count]);
        if (!at) {
            return kNone;
        }
        ++count;
        if (*at == '.') {
            ++at;
            continue;
        }
        if (*at == '\0' || *at == ' ') {
            break;
        }
        return kNone;
    }
    std::uint32_t host = 0;
    switch (count) {
    case 1:
        host = parts[0];
        break;
    case 2:
        if (parts[0] > 0xFF || parts[1] > 0xFFFFFF) {
            return kNone;
        }
        host = (parts[0] << 24) | parts[1];
        break;
    case 3:
        if (parts[0] > 0xFF || parts[1] > 0xFF || parts[2] > 0xFFFF) {
            return kNone;
        }
        host = (parts[0] << 24) | (parts[1] << 16) | parts[2];
        break;
    default:
        if (parts[0] > 0xFF || parts[1] > 0xFF || parts[2] > 0xFF || parts[3] > 0xFF) {
            return kNone;
        }
        host = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
        break;
    }
    return hostToNetworkLong(host);
}

std::string inetNtoa(std::uint32_t address) {
    char text[16];
    std::snprintf(text, sizeof text, "%u.%u.%u.%u", address & 0xFFu, (address >> 8) & 0xFFu,
                  (address >> 16) & 0xFFu, address >> 24);
    return text;
}

std::int32_t fdIsSet(std::uint32_t s, const FdSet *set) {
    if (!set) {
        return 0;
    }
    std::uint32_t count = (std::min)(set->count, kFdSetSize);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (set->sockets[i] == s) {
            return 1;
        }
    }
    return 0;
}

Winsock::Winsock(platform::Transport &transport) : transport_(transport) {}

Winsock::~Winsock() { closeAll(); }

std::int32_t Winsock::WsaGetLastError() { return tLastError; }

void Winsock::WsaSetLastError(std::int32_t error) { tLastError = error; }

std::int32_t Winsock::WsaStartup(std::uint16_t versionRequested, WsaData *data) {
    if (!data) {
        return kWsaEFault;
    }
    *data = WsaData{};
    data->version = kWinsockVersion;
    data->highVersion = kWinsockVersion;
    std::snprintf(data->description, sizeof data->description, "%s", "Winsock 1.1 on Transport");
    std::snprintf(data->systemStatus, sizeof data->systemStatus, "%s", "Running");
    data->maxSockets = 32767;
    data->maxUdpDatagram = 65467;
    const unsigned major = versionRequested & 0xFFu;
    if (major < 1) { // 1.0 and 1.1 are both served, as by the real WSOCK32
        return kWsaVerNotSupported;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ++started_;
    return 0;
}

std::int32_t Winsock::WsaCleanup() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_ == 0) {
            return fail(kWsaNotInitialised);
        }
        if (--started_ > 0) {
            return 0;
        }
    }
    closeAll();
    return 0;
}

std::uint32_t Winsock::Socket(std::int32_t family, std::int32_t type, std::int32_t protocol) {
    if (!started()) {
        return failSocket(kWsaNotInitialised);
    }
    if (family != kAfInet) {
        return failSocket(kWsaEAfNoSupport);
    }
    SocketKind kind;
    if (type == kSockStream) {
        if (protocol != 0 && protocol != kIpProtoTcp) {
            return failSocket(kWsaEProtoNoSupport);
        }
        kind = SocketKind::kTcp;
    } else if (type == kSockDgram) {
        if (protocol != 0 && protocol != kIpProtoUdp) {
            return failSocket(kWsaEProtoNoSupport);
        }
        kind = SocketKind::kUdp;
    } else {
        return failSocket(kWsaESocktNoSupport);
    }
    Result<SocketId> opened = transport_.Open(kind);
    if (!opened.Ok()) {
        return failSocket(opened.Code() == Error::kOutOfResources ? kWsaEMFile
                                                                  : wsaErrorOf(opened.Code()));
    }
    return add(opened.Value(), kind);
}

std::int32_t Winsock::CloseSocket(std::uint32_t s) {
    if (!started()) {
        return fail(kWsaNotInitialised);
    }
    SocketId id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = sockets_.find(s);
        if (found == sockets_.end()) {
            return fail(kWsaENotSock);
        }
        id = found->second.id;
        sockets_.erase(found);
    }
    Status closed = transport_.Close(id);
    if (!closed.Ok()) {
        return fail(wsaErrorOf(closed.Code()));
    }
    return 0;
}

std::int32_t Winsock::Bind(std::uint32_t s, const SockAddrIn *name, std::int32_t nameLength) {
    if (!started()) {
        return fail(kWsaNotInitialised);
    }
    std::optional<Entry> entry = lookup(s);
    if (!entry) {
        return fail(kWsaENotSock);
    }
    if (!name || nameLength < kSockAddrInSize) {
        return fail(kWsaEFault);
    }
    if (name->family != kAfInet) {
        return fail(kWsaEAfNoSupport);
    }
    Status bound = transport_.Bind(entry->id, toEndpoint(*name));
    if (!bound.Ok()) {
        return fail(bound.Code() == Error::kInvalidArgument ? kWsaEAddrNotAvail
                                                            : wsaErrorOf(bound.Code()));
    }
    return 0;
}

std::int32_t Winsock::Connect(std::uint32_t s, const SockAddrIn *name, std::int32_t nameLength) {
    if (!started()) {
        return fail(kWsaNotInitialised);
    }
    std::optional<Entry> entry = lookup(s);
    if (!entry) {
        return fail(kWsaENotSock);
    }
    if (!name || nameLength < kSockAddrInSize) {
        return fail(kWsaEFault);
    }
    if (name->family != kAfInet) {
        return fail(kWsaEAfNoSupport);
    }
    Status connected = transport_.Connect(entry->id, toEndpoint(*name));
    if (connected.Ok()) {
        setPending(s, false, 0);
        return 0;
    }
    switch (connected.Code()) {
    case Error::kInProgress:
        if (entry->connectPending) {
            return fail(kWsaEAlready);
        }
        setPending(s, true, 0);
        return fail(kWsaEWouldBlock);
    case Error::kWrongState:
        setPending(s, false, 0);
        return fail(kWsaEIsConn);
    case Error::kInvalidArgument:
        return fail(kWsaEAddrNotAvail);
    default: {
        std::int32_t error = blockedError(s, connected.Code(), wsaErrorOf(connected.Code()));
        setPending(s, false, error);
        return fail(error);
    }
    }
}

std::int32_t Winsock::Listen(std::uint32_t s, std::int32_t backlog) {
    if (!started()) {
        return fail(kWsaNotInitialised);
    }
    std::optional<Entry> entry = lookup(s);
    if (!entry) {
        return fail(kWsaENotSock);
    }
    if (entry->kind != SocketKind::kTcp) {
        return fail(kWsaEOpNotSupp);
    }
    Status listening =
        transport_.Listen(entry->id, static_cast<std::uint32_t>((std::max)(backlog, 0)));
    if (!listening.Ok()) {
        return fail(wsaErrorOf(listening.Code()));
    }
    return 0;
}

std::uint32_t Winsock::Accept(std::uint32_t s, SockAddrIn *address, std::int32_t *addressLength) {
    if (!started()) {
        return failSocket(kWsaNotInitialised);
    }
    std::optional<Entry> entry = lookup(s);
    if (!entry) {
        return failSocket(kWsaENotSock);
    }
    if (!outAddressFits(address, addressLength)) {
        return failSocket(kWsaEFault);
    }
    if (entry->kind != SocketKind::kTcp) {
        return failSocket(kWsaEOpNotSupp);
    }
    Result<platform::Accepted> accepted = transport_.Accept(entry->id);
    if (!accepted.Ok()) {
        return failSocket(blockedError(s, accepted.Code(), wsaErrorOf(accepted.Code())));
    }
    std::uint32_t guest = add(accepted.Value().socket, SocketKind::kTcp);
    writeAddress(address, addressLength, accepted.Value().peer);
    return guest;
}

std::int32_t Winsock::Send(std::uint32_t s, const void *buffer, std::int32_t length,
                           std::int32_t flags) {
    if (!started()) {
        return fail(kWsaNotInitialised);
    }
    std::optional<Entry> entry = lookup(s);
    if (!entry) {
        return fail(kWsaENotSock);
    }
    if (length < 0 || (!buffer && length > 0)) {
        return fail(kWsaEFault);
    }
    if ((flags & kMsgOob) != 0 || (flags & ~(kMsgOob | kMsgDontRoute)) != 0) {
        return fail(kWsaEOpNotSupp);
    }
    Result<std::size_t> sent =
        transport_.Send(entry->id, buffer, static_cast<std::size_t>(length));
    if (!sent.Ok()) {
        if (sent.Code() == Error::kInvalidArgument) {
            return fail(kWsaEMsgSize);
        }
        return fail(blockedError(s, sent.Code(), wsaErrorOf(sent.Code())));
    }
    return static_cast<std::int32_t>(sent.Value());
}

std::int32_t Winsock::Recv(std::uint32_t s, void *buffer, std::int32_t length,
                           std::int32_t flags) {
    if (!started()) {
        return fail(kWsaNotInitialised);
    }
    std::optional<Entry> entry = lookup(s);
    if (!entry) {
        return fail(kWsaENotSock);
    }
    if (length < 0 || (!buffer && length > 0)) {
        return fail(kWsaEFault);
    }
    if (flags != 0) {
        return fail(kWsaEOpNotSupp); // MSG_PEEK and MSG_OOB: the Transport has neither
    }
    Result<std::size_t> got = transport_.Receive(entry->id, buffer, static_cast<std::size_t>(length));
    if (!got.Ok()) {
        return fail(blockedError(s, got.Code(), wsaErrorOf(got.Code())));
    }
    return static_cast<std::int32_t>(got.Value());
}

std::int32_t Winsock::SendTo(std::uint32_t s, const void *buffer, std::int32_t length,
                             std::int32_t flags, const SockAddrIn *to, std::int32_t toLength) {
    if (!to) {
        return Send(s, buffer, length, flags);
    }
    if (!started()) {
        return fail(kWsaNotInitialised);
    }
    std::optional<Entry> entry = lookup(s);
    if (!entry) {
        return fail(kWsaENotSock);
    }
    if (length < 0 || (!buffer && length > 0) || toLength < kSockAddrInSize) {
        return fail(kWsaEFault);
    }
    if ((flags & kMsgOob) != 0 || (flags & ~(kMsgOob | kMsgDontRoute)) != 0) {
        return fail(kWsaEOpNotSupp);
    }
    if (entry->kind == SocketKind::kTcp) {
        return Send(s, buffer, length, flags); // a stream socket ignores the address
    }
    if (to->family != kAfInet) {
        return fail(kWsaEAfNoSupport);
    }
    Result<std::size_t> sent =
        transport_.SendTo(entry->id, buffer, static_cast<std::size_t>(length), toEndpoint(*to));
    if (!sent.Ok()) {
        if (sent.Code() == Error::kInvalidArgument) {
            return fail(kWsaEMsgSize);
        }
        return fail(blockedError(s, sent.Code(), wsaErrorOf(sent.Code())));
    }
    return static_cast<std::int32_t>(sent.Value());
}

std::int32_t Winsock::RecvFrom(std::uint32_t s, void *buffer, std::int32_t length,
                               std::int32_t flags, SockAddrIn *from, std::int32_t *fromLength) {
    if (!started()) {
        return fail(kWsaNotInitialised);
    }
    std::optional<Entry> entry = lookup(s);
    if (!entry) {
        return fail(kWsaENotSock);
    }
    if (length < 0 || (!buffer && length > 0)) {
        return fail(kWsaEFault);
    }
    if (flags != 0) {
        return fail(kWsaEOpNotSupp);
    }
    if (entry->kind == SocketKind::kTcp) {
        // A stream socket leaves `from` alone.
        Result<std::size_t> got =
            transport_.Receive(entry->id, buffer, static_cast<std::size_t>(length));
        if (!got.Ok()) {
            return fail(blockedError(s, got.Code(), wsaErrorOf(got.Code())));
        }
        return static_cast<std::int32_t>(got.Value());
    }
    if (!outAddressFits(from, fromLength)) {
        return fail(kWsaEFault);
    }
    Result<platform::Received> got =
        transport_.ReceiveFrom(entry->id, buffer, static_cast<std::size_t>(length));
    if (!got.Ok()) {
        return fail(blockedError(s, got.Code(), wsaErrorOf(got.Code())));
    }
    writeAddress(from, fromLength, got.Value().from);
    return static_cast<std::int32_t>(got.Value().bytes);
}

std::int32_t Winsock::Select(std::int32_t, FdSet *readfds, FdSet *writefds, FdSet *exceptfds,
                             const TimeVal *timeout) {
    if (!started()) {
        return fail(kWsaNotInitialised);
    }
    std::optional<std::uint32_t> timeoutMs;
    if (timeout) {
        if (timeout->seconds < 0 || timeout->microseconds < 0) {
            return fail(kWsaEInval);
        }
        // A part of a millisecond waits a whole one, so a short timeout is not a poll.
        std::uint64_t ms = static_cast<std::uint64_t>(timeout->seconds) * 1000u +
                           (static_cast<std::uint64_t>(timeout->microseconds) + 999u) / 1000u;
        timeoutMs = static_cast<std::uint32_t>(
            (std::min)(ms, std::uint64_t{std::numeric_limits<std::uint32_t>::max() - 1}));
    }
    FdSet *guest[3] = {readfds, writefds, exceptfds};
    SelectSet sets;
    std::vector<SocketId> *lists[3] = {&sets.readable, &sets.writable, &sets.errored};
    std::vector<std::uint32_t> pending; // guest sockets whose connect has not been seen to end
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int k = 0; k < 3; ++k) {
            if (!guest[k]) {
                continue;
            }
            std::uint32_t count = (std::min)(guest[k]->count, kFdSetSize);
            for (std::uint32_t i = 0; i < count; ++i) {
                auto found = sockets_.find(guest[k]->sockets[i]);
                if (found == sockets_.end()) {
                    return fail(kWsaENotSock);
                }
                lists[k]->push_back(found->second.id);
                if (found->second.connectPending) {
                    pending.push_back(found->first);
                }
            }
        }
    }
    if (sets.readable.empty() && sets.writable.empty() && sets.errored.empty()) {
        return fail(kWsaEInval);
    }
    std::vector<SocketId> asked[3] = {sets.readable, sets.writable, sets.errored};
    Result<std::size_t> ready = transport_.Select(sets, timeoutMs);
    if (!ready.Ok()) {
        if (ready.Code() == Error::kInvalidHandle) {
            return fail(kWsaENotSock);
        }
        if (ready.Code() == Error::kNotConnected) {
            return fail(kWsaEIntr); // a socket was closed while the call waited
        }
        return fail(wsaErrorOf(ready.Code()));
    }
    // Each guest set keeps, in its order, the entries whose socket is ready.
    std::int32_t total = 0;
    for (int k = 0; k < 3; ++k) {
        if (!guest[k]) {
            continue;
        }
        std::vector<SocketId> left = *lists[k];
        std::uint32_t count = (std::min)(guest[k]->count, kFdSetSize);
        std::uint32_t kept = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            auto at = std::find(left.begin(), left.end(), asked[k][i]);
            if (at == left.end()) {
                continue;
            }
            left.erase(at);
            guest[k]->sockets[kept++] = guest[k]->sockets[i];
        }
        guest[k]->count = kept;
        total += static_cast<std::int32_t>(kept);
    }
    // A connect that ended: writable is success, exceptfds failure (SO_ERROR then says why).
    for (std::uint32_t s : pending) {
        if (fdIsSet(s, writefds)) {
            setPending(s, false, 0);
        } else if (fdIsSet(s, exceptfds)) {
            setPending(s, false, kWsaEConnRefused);
        }
    }
    return total;
}

std::int32_t Winsock::Shutdown(std::uint32_t s, std::int32_t how) {
    if (!started()) {
        return fail(kWsaNotInitialised);
    }
    std::optional<Entry> entry = lookup(s);
    if (!entry) {
        return fail(kWsaENotSock);
    }
    platform::ShutdownHow mode;
    switch (how) {
    case kSdReceive:
        mode = platform::ShutdownHow::kReceive;
        break;
    case kSdSend:
        mode = platform::ShutdownHow::kSend;
        break;
    case kSdBoth:
        mode = platform::ShutdownHow::kBoth;
        break;
    default:
        return fail(kWsaEInval);
    }
    Status done = transport_.Shutdown(entry->id, mode);
    if (!done.Ok()) {
        return fail(wsaErrorOf(done.Code()));
    }
    return 0;
}

std::int32_t Winsock::GetPeerName(std::uint32_t s, SockAddrIn *name, std::int32_t *nameLength) {
    if (!started()) {
        return fail(kWsaNotInitialised);
    }
    std::optional<Entry> entry = lookup(s);
    if (!entry) {
        return fail(kWsaENotSock);
    }
    if (!name || !outAddressFits(name, nameLength)) {
        return fail(kWsaEFault);
    }
    Result<Endpoint> peer = transport_.PeerName(entry->id);
    if (!peer.Ok()) {
        return fail(wsaErrorOf(peer.Code()));
    }
    writeAddress(name, nameLength, peer.Value());
    return 0;
}

std::int32_t Winsock::GetSockName(std::uint32_t s, SockAddrIn *name, std::int32_t *nameLength) {
    if (!started()) {
        return fail(kWsaNotInitialised);
    }
    std::optional<Entry> entry = lookup(s);
    if (!entry) {
        return fail(kWsaENotSock);
    }
    if (!name || !outAddressFits(name, nameLength)) {
        return fail(kWsaEFault);
    }
    Result<Endpoint> local = transport_.LocalName(entry->id);
    if (!local.Ok()) {
        return fail(wsaErrorOf(local.Code()));
    }
    writeAddress(name, nameLength, local.Value());
    return 0;
}

std::int32_t Winsock::IoctlSocket(std::uint32_t s, std::uint32_t command,
                                  std::uint32_t *argument) {
    if (!started()) {
        return fail(kWsaNotInitialised);
    }
    std::optional<Entry> entry = lookup(s);
    if (!entry) {
        return fail(kWsaENotSock);
    }
    if (!argument) {
        return fail(kWsaEFault);
    }
    switch (command) {
    case kFionbio: {
        Status set = transport_.SetOption(entry->id, SocketOption::kNonBlocking,
                                          *argument != 0 ? 1 : 0);
        if (!set.Ok()) {
            return fail(wsaErrorOf(set.Code()));
        }
        return 0;
    }
    case kFionread: {
        Result<std::int32_t> readable = transport_.GetOption(entry->id, SocketOption::kBytesReadable);
        if (!readable.Ok()) {
            return fail(wsaErrorOf(readable.Code()));
        }
        *argument = static_cast<std::uint32_t>(readable.Value());
        return 0;
    }
    case kSiocatmark:
        *argument = 1; // no out-of-band data is ever pending
        return 0;
    default:
        return fail(kWsaEInval);
    }
}

std::int32_t Winsock::SetSockOpt(std::uint32_t s, std::int32_t level, std::int32_t name,
                                 const std::uint32_t *value, std::int32_t length) {
    if (!started()) {
        return fail(kWsaNotInitialised);
    }
    std::optional<Entry> entry = lookup(s);
    if (!entry) {
        return fail(kWsaENotSock);
    }
    if (!value || length < 4) {
        return fail(kWsaEFault);
    }
    const bool tcp = entry->kind == SocketKind::kTcp;
    auto apply = [&](SocketOption option, std::int32_t v) -> std::int32_t {
        Status set = transport_.SetOption(entry->id, option, v);
        if (!set.Ok()) {
            return fail(wsaErrorOf(set.Code()));
        }
        return 0;
    };
    const std::int32_t flag = *value != 0 ? 1 : 0;
    if (level == kIpProtoTcp) {
        if (name != kTcpNoDelay || !tcp) {
            return fail(kWsaENoProtoOpt);
        }
        return apply(SocketOption::kNoDelay, flag);
    }
    if (level != kSolSocket) {
        return fail(kWsaEInval);
    }
    switch (name) {
    case kSoReuseAddr:
        return apply(SocketOption::kReuseAddress, flag);
    case kSoKeepAlive:
        return tcp ? apply(SocketOption::kKeepAlive, flag) : fail(kWsaENoProtoOpt);
    case kSoBroadcast:
        return tcp ? fail(kWsaENoProtoOpt) : apply(SocketOption::kBroadcast, flag);
    case kSoLinger: {
        if (!tcp) {
            return fail(kWsaENoProtoOpt);
        }
        const std::uint32_t onOff = *value & 0xFFFFu;
        const std::int32_t seconds = static_cast<std::int32_t>(*value >> 16);
        return apply(SocketOption::kLingerSeconds, onOff != 0 ? seconds : -1);
    }
    case kSoDontLinger: {
        if (!tcp) {
            return fail(kWsaENoProtoOpt);
        }
        if (flag) {
            return apply(SocketOption::kLingerSeconds, -1);
        }
        // Lingering on again keeps the timeout l_linger had (zero if it never had one).
        Result<std::int32_t> current = transport_.GetOption(entry->id, SocketOption::kLingerSeconds);
        if (!current.Ok()) {
            return fail(wsaErrorOf(current.Code()));
        }
        return apply(SocketOption::kLingerSeconds, (std::max)(current.Value(), 0));
    }
    case kSoSndBuf:
        return apply(SocketOption::kSendBuffer, static_cast<std::int32_t>(*value));
    case kSoRcvBuf:
        return apply(SocketOption::kReceiveBuffer, static_cast<std::int32_t>(*value));
    default:
        return fail(kWsaENoProtoOpt);
    }
}

std::int32_t Winsock::GetSockOpt(std::uint32_t s, std::int32_t level, std::int32_t name,
                                 std::uint32_t *value, std::int32_t *length) {
    if (!started()) {
        return fail(kWsaNotInitialised);
    }
    std::optional<Entry> entry = lookup(s);
    if (!entry) {
        return fail(kWsaENotSock);
    }
    if (!value || !length || *length < 4) {
        return fail(kWsaEFault);
    }
    const bool tcp = entry->kind == SocketKind::kTcp;
    auto answer = [&](std::uint32_t v) -> std::int32_t {
        *value = v;
        *length = 4;
        return 0;
    };
    auto read = [&](SocketOption option, std::int32_t &out) -> bool {
        Result<std::int32_t> got = transport_.GetOption(entry->id, option);
        if (!got.Ok()) {
            fail(wsaErrorOf(got.Code()));
            return false;
        }
        out = got.Value();
        return true;
    };
    std::int32_t v = 0;
    if (level == kIpProtoTcp) {
        if (name != kTcpNoDelay || !tcp) {
            return fail(kWsaENoProtoOpt);
        }
        return read(SocketOption::kNoDelay, v) ? answer(static_cast<std::uint32_t>(v))
                                               : kSocketError;
    }
    if (level != kSolSocket) {
        return fail(kWsaEInval);
    }
    SocketOption option;
    switch (name) {
    case kSoType:
        return answer(tcp ? kSockStream : kSockDgram);
    case kSoError:
        return answer(static_cast<std::uint32_t>(entry->socketError));
    case kSoReuseAddr:
        option = SocketOption::kReuseAddress;
        break;
    case kSoKeepAlive:
        if (!tcp) {
            return fail(kWsaENoProtoOpt);
        }
        option = SocketOption::kKeepAlive;
        break;
    case kSoBroadcast:
        if (tcp) {
            return fail(kWsaENoProtoOpt);
        }
        option = SocketOption::kBroadcast;
        break;
    case kSoSndBuf:
        option = SocketOption::kSendBuffer;
        break;
    case kSoRcvBuf:
        option = SocketOption::kReceiveBuffer;
        break;
    case kSoLinger:
    case kSoDontLinger:
        if (!tcp) {
            return fail(kWsaENoProtoOpt);
        }
        if (!read(SocketOption::kLingerSeconds, v)) {
            return kSocketError;
        }
        if (name == kSoDontLinger) {
            return answer(v < 0 ? 1u : 0u);
        }
        return answer(v < 0 ? 0u : (1u | (static_cast<std::uint32_t>(v) << 16)));
    default:
        return fail(kWsaENoProtoOpt);
    }
    return read(option, v) ? answer(static_cast<std::uint32_t>(v)) : kSocketError;
}

std::optional<HostEntry> Winsock::GetHostByName(const char *name) {
    if (!started()) {
        fail(kWsaNotInitialised);
        return std::nullopt;
    }
    std::string asked = name ? name : "";
    if (asked.empty()) {
        Result<std::string> self = transport_.HostName();
        if (!self.Ok()) {
            fail(kWsaNoRecovery);
            return std::nullopt;
        }
        asked = self.Value();
    }
    Result<std::vector<std::uint32_t>> found = transport_.Resolve(asked);
    if (!found.Ok()) {
        switch (found.Code()) {
        case Error::kNotFound:
        case Error::kInvalidArgument:
            fail(kWsaHostNotFound);
            break;
        case Error::kTimedOut:
        case Error::kHostUnreachable:
            fail(kWsaTryAgain);
            break;
        case Error::kOutOfResources:
            fail(kWsaENoBufs);
            break;
        default:
            fail(kWsaNoRecovery);
            break;
        }
        return std::nullopt;
    }
    if (found.Value().empty()) {
        fail(kWsaNoData); // the name exists but has no IPv4 address
        return std::nullopt;
    }
    return HostEntry{asked, found.Value()};
}

std::int32_t Winsock::GetHostName(char *name, std::int32_t length) {
    if (!started()) {
        return fail(kWsaNotInitialised);
    }
    if (!name || length <= 0) {
        return fail(kWsaEFault);
    }
    Result<std::string> self = transport_.HostName();
    if (!self.Ok()) {
        return fail(kWsaENetDown);
    }
    if (self.Value().size() + 1 > static_cast<std::size_t>(length)) {
        return fail(kWsaEFault);
    }
    std::memcpy(name, self.Value().c_str(), self.Value().size() + 1);
    return 0;
}

bool Winsock::started() {
    std::lock_guard<std::mutex> lock(mutex_);
    return started_ > 0;
}

std::optional<Winsock::Entry> Winsock::lookup(std::uint32_t s) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = sockets_.find(s);
    if (found == sockets_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::uint32_t Winsock::add(SocketId id, SocketKind kind) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (;;) {
        std::uint32_t s = nextSocket_;
        nextSocket_ += 4; // Winsock's handles are multiples of 4
        if (nextSocket_ >= 0x7FFFFFF0u) {
            nextSocket_ = 0x100;
        }
        if (sockets_.find(s) == sockets_.end()) {
            Entry entry;
            entry.id = id;
            entry.kind = kind;
            sockets_.emplace(s, entry);
            return s;
        }
    }
}

void Winsock::setPending(std::uint32_t s, bool pending, std::int32_t socketError) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = sockets_.find(s);
    if (found != sockets_.end()) {
        found->second.connectPending = pending;
        found->second.socketError = socketError;
    }
}

std::int32_t Winsock::blockedError(std::uint32_t s, Error error, std::int32_t otherwise) {
    if (error == Error::kNotConnected && !lookup(s)) {
        return kWsaEIntr;
    }
    return otherwise;
}

void Winsock::closeAll() {
    std::vector<SocketId> open;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &socket : sockets_) {
            open.push_back(socket.second.id);
        }
        sockets_.clear();
    }
    for (SocketId id : open) {
        (void)transport_.Close(id);
    }
}

} // namespace sfcop::hle::winsock
