// The C entry points of WinsockApi.h, on the process's module instance. No C++ exception leaves
// them (CONVENTIONS §4.2): one that would becomes WSAENOBUFS, the error Winsock gives when it
// runs out of memory.
#include "hle/winsock/WinsockApi.h"

#include "hle/winsock/Winsock.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <string>

namespace sfcop::hle::winsock {
namespace {

struct Instance {
    std::mutex mutex;
    std::unique_ptr<platform::Transport> transport;
    std::unique_ptr<Winsock> winsock;
    std::map<std::string, std::string> aliases;
};

Instance &instance() {
    static Instance one;
    return one;
}

template <typename Call> auto guarded(decltype(std::declval<Call>()()) onFailure, Call call) {
    try {
        return call();
    } catch (...) {
        Winsock::WsaSetLastError(kWsaENoBufs);
        return onFailure;
    }
}

} // namespace

Winsock &guestWinsock() {
    Instance &one = instance();
    std::lock_guard<std::mutex> lock(one.mutex);
    if (!one.winsock) {
        one.transport = platform::makeTransport();
        one.winsock = std::make_unique<Winsock>(*one.transport);
        one.winsock->SetHostAliases(one.aliases);
    }
    return *one.winsock;
}

void installGuestWinsock(std::unique_ptr<platform::Transport> transport) {
    Instance &one = instance();
    std::lock_guard<std::mutex> lock(one.mutex);
    one.winsock.reset();
    one.transport = std::move(transport);
    one.winsock = std::make_unique<Winsock>(*one.transport);
    one.winsock->SetHostAliases(one.aliases);
}

void setGuestHostAliases(const std::map<std::string, std::string> &aliases) {
    Instance &one = instance();
    std::lock_guard<std::mutex> lock(one.mutex);
    one.aliases = aliases;
    if (one.winsock) {
        one.winsock->SetHostAliases(one.aliases);
    }
}

} // namespace sfcop::hle::winsock

using sfcop::hle::winsock::guarded;
using sfcop::hle::winsock::guestWinsock;
using sfcop::hle::winsock::kInvalidSocket;
using sfcop::hle::winsock::kSocketError;
using sfcop::hle::winsock::Winsock;

extern "C" {

int32_t sfcop_winsock_WSAStartup(uint16_t versionRequested, WinsockData *data) {
    return guarded(sfcop::hle::winsock::kWsaENoBufs,
                   [&] { return guestWinsock().WsaStartup(versionRequested, data); });
}

int32_t sfcop_winsock_WSACleanup(void) {
    return guarded(kSocketError, [&] { return guestWinsock().WsaCleanup(); });
}

int32_t sfcop_winsock_WSAGetLastError(void) { return Winsock::WsaGetLastError(); }

void sfcop_winsock_WSASetLastError(int32_t error) { Winsock::WsaSetLastError(error); }

uint32_t sfcop_winsock_socket(int32_t family, int32_t type, int32_t protocol) {
    return guarded(kInvalidSocket, [&] { return guestWinsock().Socket(family, type, protocol); });
}

int32_t sfcop_winsock_closesocket(uint32_t s) {
    return guarded(kSocketError, [&] { return guestWinsock().CloseSocket(s); });
}

int32_t sfcop_winsock_bind(uint32_t s, const WinsockSockAddr *name, int32_t nameLength) {
    return guarded(kSocketError, [&] { return guestWinsock().Bind(s, name, nameLength); });
}

int32_t sfcop_winsock_connect(uint32_t s, const WinsockSockAddr *name, int32_t nameLength) {
    return guarded(kSocketError, [&] { return guestWinsock().Connect(s, name, nameLength); });
}

int32_t sfcop_winsock_listen(uint32_t s, int32_t backlog) {
    return guarded(kSocketError, [&] { return guestWinsock().Listen(s, backlog); });
}

uint32_t sfcop_winsock_accept(uint32_t s, WinsockSockAddr *address, int32_t *addressLength) {
    return guarded(kInvalidSocket,
                   [&] { return guestWinsock().Accept(s, address, addressLength); });
}

int32_t sfcop_winsock_send(uint32_t s, const void *buffer, int32_t length, int32_t flags) {
    return guarded(kSocketError, [&] { return guestWinsock().Send(s, buffer, length, flags); });
}

int32_t sfcop_winsock_recv(uint32_t s, void *buffer, int32_t length, int32_t flags) {
    return guarded(kSocketError, [&] { return guestWinsock().Recv(s, buffer, length, flags); });
}

int32_t sfcop_winsock_sendto(uint32_t s, const void *buffer, int32_t length, int32_t flags,
                             const WinsockSockAddr *to, int32_t toLength) {
    return guarded(kSocketError,
                   [&] { return guestWinsock().SendTo(s, buffer, length, flags, to, toLength); });
}

int32_t sfcop_winsock_recvfrom(uint32_t s, void *buffer, int32_t length, int32_t flags,
                               WinsockSockAddr *from, int32_t *fromLength) {
    return guarded(kSocketError, [&] {
        return guestWinsock().RecvFrom(s, buffer, length, flags, from, fromLength);
    });
}

int32_t sfcop_winsock_select(int32_t nfds, WinsockFdSet *readfds, WinsockFdSet *writefds,
                             WinsockFdSet *exceptfds, const WinsockTimeVal *timeout) {
    return guarded(kSocketError, [&] {
        return guestWinsock().Select(nfds, readfds, writefds, exceptfds, timeout);
    });
}

int32_t sfcop_winsock_fd_is_set(uint32_t s, const WinsockFdSet *set) {
    return sfcop::hle::winsock::fdIsSet(s, set);
}

int32_t sfcop_winsock_shutdown(uint32_t s, int32_t how) {
    return guarded(kSocketError, [&] { return guestWinsock().Shutdown(s, how); });
}

int32_t sfcop_winsock_getpeername(uint32_t s, WinsockSockAddr *name, int32_t *nameLength) {
    return guarded(kSocketError, [&] { return guestWinsock().GetPeerName(s, name, nameLength); });
}

int32_t sfcop_winsock_getsockname(uint32_t s, WinsockSockAddr *name, int32_t *nameLength) {
    return guarded(kSocketError, [&] { return guestWinsock().GetSockName(s, name, nameLength); });
}

int32_t sfcop_winsock_ioctlsocket(uint32_t s, int32_t command, uint32_t *argument) {
    return guarded(kSocketError, [&] {
        return guestWinsock().IoctlSocket(s, static_cast<uint32_t>(command), argument);
    });
}

int32_t sfcop_winsock_setsockopt(uint32_t s, int32_t level, int32_t name, const uint32_t *value,
                                 int32_t length) {
    return guarded(kSocketError,
                   [&] { return guestWinsock().SetSockOpt(s, level, name, value, length); });
}

int32_t sfcop_winsock_getsockopt(uint32_t s, int32_t level, int32_t name, uint32_t *value,
                                 int32_t *length) {
    return guarded(kSocketError,
                   [&] { return guestWinsock().GetSockOpt(s, level, name, value, length); });
}

int32_t sfcop_winsock_gethostbyname(const char *name, WinsockHostEntry *entry) {
    return guarded(0, [&]() -> int32_t {
        std::optional<sfcop::hle::winsock::HostEntry> found = guestWinsock().GetHostByName(name);
        if (!found) {
            return 0;
        }
        if (entry) {
            std::memset(entry, 0, sizeof *entry);
            std::size_t length = (std::min)(found->name.size(), sizeof entry->name - 1);
            std::memcpy(entry->name, found->name.data(), length);
            std::size_t count = (std::min)(found->addresses.size(),
                                           sizeof entry->addresses / sizeof entry->addresses[0]);
            std::copy_n(found->addresses.begin(), count, entry->addresses);
            entry->count = static_cast<uint32_t>(count);
        }
        return 1;
    });
}

int32_t sfcop_winsock_gethostname(char *name, int32_t length) {
    return guarded(kSocketError, [&] { return guestWinsock().GetHostName(name, length); });
}

uint16_t sfcop_winsock_htons(uint16_t value) {
    return sfcop::hle::winsock::hostToNetworkShort(value);
}

uint16_t sfcop_winsock_ntohs(uint16_t value) {
    return sfcop::hle::winsock::networkToHostShort(value);
}

uint32_t sfcop_winsock_htonl(uint32_t value) {
    return sfcop::hle::winsock::hostToNetworkLong(value);
}

uint32_t sfcop_winsock_ntohl(uint32_t value) {
    return sfcop::hle::winsock::networkToHostLong(value);
}

uint32_t sfcop_winsock_inet_addr(const char *text) {
    return sfcop::hle::winsock::inetAddr(text);
}

void sfcop_winsock_inet_ntoa(uint32_t address, char out[16]) {
    std::string text = sfcop::hle::winsock::inetNtoa(address);
    std::memcpy(out, text.c_str(), text.size() + 1);
}

} // extern "C"
