// The WSOCK32 module: Winsock 1.1 as the game calls it, on the Transport component (DESIGN §4.7,
// §6.1, §6.2). The game's SOCKETs are guest integers that this module maps to Transport sockets;
// the structures it passes are the host-side forms of WinsockApi.h, which the guest shims fill
// from guest memory and write back.
//
// Every method returns what the Winsock call returns and, on failure, leaves the Winsock error
// for WsaGetLastError() on the calling thread (that error is per thread, as Winsock's is, and
// shared by all instances). Calls may come from any thread and block as Winsock's do; none holds
// a lock while it waits on the Transport.
#pragma once

#include "hle/winsock/WinsockApi.h"
#include "platform/Transport/Transport.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sfcop::hle::winsock {

using SockAddrIn = ::WinsockSockAddr;
using FdSet = ::WinsockFdSet;
using TimeVal = ::WinsockTimeVal;
using WsaData = ::WinsockData;

constexpr std::uint32_t kInvalidSocket = 0xFFFFFFFF;
constexpr std::int32_t kSocketError = -1;
constexpr std::uint32_t kFdSetSize = 64;
constexpr std::int32_t kSockAddrInSize = 16;

constexpr std::int16_t kAfInet = 2;
constexpr std::int32_t kSockStream = 1;
constexpr std::int32_t kSockDgram = 2;
constexpr std::int32_t kIpProtoTcp = 6;
constexpr std::int32_t kIpProtoUdp = 17;

constexpr std::int32_t kMsgOob = 0x1;
constexpr std::int32_t kMsgPeek = 0x2;
constexpr std::int32_t kMsgDontRoute = 0x4;

constexpr std::int32_t kSdReceive = 0;
constexpr std::int32_t kSdSend = 1;
constexpr std::int32_t kSdBoth = 2;

constexpr std::int32_t kSolSocket = 0xFFFF;
constexpr std::int32_t kSoReuseAddr = 0x0004;
constexpr std::int32_t kSoKeepAlive = 0x0008;
constexpr std::int32_t kSoBroadcast = 0x0020;
constexpr std::int32_t kSoLinger = 0x0080;
constexpr std::int32_t kSoDontLinger = ~0x0080;
constexpr std::int32_t kSoSndBuf = 0x1001;
constexpr std::int32_t kSoRcvBuf = 0x1002;
constexpr std::int32_t kSoError = 0x1007;
constexpr std::int32_t kSoType = 0x1008;
constexpr std::int32_t kTcpNoDelay = 0x0001;

constexpr std::uint32_t kFionbio = 0x8004667E;
constexpr std::uint32_t kFionread = 0x4004667F;
constexpr std::uint32_t kSiocatmark = 0x40047307;

constexpr std::uint16_t kWinsockVersion = 0x0101; // 1.1: what WSOCK32 provides

// The Winsock error numbers this module reports.
constexpr std::int32_t kWsaEIntr = 10004;
constexpr std::int32_t kWsaEAcces = 10013;
constexpr std::int32_t kWsaEFault = 10014;
constexpr std::int32_t kWsaEInval = 10022;
constexpr std::int32_t kWsaEMFile = 10024;
constexpr std::int32_t kWsaEWouldBlock = 10035;
constexpr std::int32_t kWsaEAlready = 10037;
constexpr std::int32_t kWsaENotSock = 10038;
constexpr std::int32_t kWsaEMsgSize = 10040;
constexpr std::int32_t kWsaENoProtoOpt = 10042;
constexpr std::int32_t kWsaEProtoNoSupport = 10043;
constexpr std::int32_t kWsaESocktNoSupport = 10044;
constexpr std::int32_t kWsaEOpNotSupp = 10045;
constexpr std::int32_t kWsaEAfNoSupport = 10047;
constexpr std::int32_t kWsaEAddrInUse = 10048;
constexpr std::int32_t kWsaEAddrNotAvail = 10049;
constexpr std::int32_t kWsaENetDown = 10050;
constexpr std::int32_t kWsaEConnReset = 10054;
constexpr std::int32_t kWsaENoBufs = 10055;
constexpr std::int32_t kWsaEIsConn = 10056;
constexpr std::int32_t kWsaENotConn = 10057;
constexpr std::int32_t kWsaETimedOut = 10060;
constexpr std::int32_t kWsaEConnRefused = 10061;
constexpr std::int32_t kWsaEHostUnreach = 10065;
constexpr std::int32_t kWsaVerNotSupported = 10092;
constexpr std::int32_t kWsaNotInitialised = 10093;
constexpr std::int32_t kWsaHostNotFound = 11001;
constexpr std::int32_t kWsaTryAgain = 11002;
constexpr std::int32_t kWsaNoRecovery = 11003;
constexpr std::int32_t kWsaNoData = 11004;

// The Winsock error a Transport error becomes where a call has no reason to differ.
std::int32_t wsaErrorOf(platform::Error error);

// What GetHostByName() found; addresses in host byte order.
struct HostEntry {
    std::string name;
    std::vector<std::uint32_t> addresses;
};

// The stateless functions. The guest is little-endian x86, so the byte-order functions always
// swap; an in_addr by value holds the first octet in its low byte.
constexpr std::uint16_t hostToNetworkShort(std::uint16_t value) {
    return static_cast<std::uint16_t>((value >> 8) | (value << 8));
}
constexpr std::uint16_t networkToHostShort(std::uint16_t value) { return hostToNetworkShort(value); }
constexpr std::uint32_t hostToNetworkLong(std::uint32_t value) {
    return (value >> 24) | ((value >> 8) & 0xFF00u) | ((value << 8) & 0xFF0000u) | (value << 24);
}
constexpr std::uint32_t networkToHostLong(std::uint32_t value) { return hostToNetworkLong(value); }
// inet_addr: "a.b.c.d" and the shorter forms "a.b.c", "a.b", "a", each part decimal, octal
// (leading 0) or hex (0x); text after a space is ignored. 0xFFFFFFFF (INADDR_NONE) if malformed.
std::uint32_t inetAddr(const char *text);
// inet_ntoa: "a.b.c.d".
std::string inetNtoa(std::uint32_t address);
// __WSAFDIsSet: nonzero if `s` is among the first `count` (at most 64) entries of `set`.
std::int32_t fdIsSet(std::uint32_t s, const FdSet *set);

class Winsock {
public:
    explicit Winsock(platform::Transport &transport);
    ~Winsock(); // closes every socket the guest left open
    Winsock(const Winsock &) = delete;
    Winsock &operator=(const Winsock &) = delete;

    // WSAStartup returns its error instead of setting it.
    std::int32_t WsaStartup(std::uint16_t versionRequested, WsaData *data);
    std::int32_t WsaCleanup();
    static std::int32_t WsaGetLastError();
    static void WsaSetLastError(std::int32_t error);

    std::uint32_t Socket(std::int32_t family, std::int32_t type, std::int32_t protocol);
    std::int32_t CloseSocket(std::uint32_t s);
    std::int32_t Bind(std::uint32_t s, const SockAddrIn *name, std::int32_t nameLength);
    std::int32_t Connect(std::uint32_t s, const SockAddrIn *name, std::int32_t nameLength);
    std::int32_t Listen(std::uint32_t s, std::int32_t backlog);
    std::uint32_t Accept(std::uint32_t s, SockAddrIn *address, std::int32_t *addressLength);
    std::int32_t Send(std::uint32_t s, const void *buffer, std::int32_t length,
                      std::int32_t flags);
    std::int32_t Recv(std::uint32_t s, void *buffer, std::int32_t length, std::int32_t flags);
    std::int32_t SendTo(std::uint32_t s, const void *buffer, std::int32_t length,
                        std::int32_t flags, const SockAddrIn *to, std::int32_t toLength);
    std::int32_t RecvFrom(std::uint32_t s, void *buffer, std::int32_t length, std::int32_t flags,
                          SockAddrIn *from, std::int32_t *fromLength);
    // nfds is ignored, as Winsock ignores it. A NULL timeout waits forever; the sets keep only
    // their ready sockets.
    std::int32_t Select(std::int32_t nfds, FdSet *readfds, FdSet *writefds, FdSet *exceptfds,
                        const TimeVal *timeout);
    std::int32_t Shutdown(std::uint32_t s, std::int32_t how);
    std::int32_t GetPeerName(std::uint32_t s, SockAddrIn *name, std::int32_t *nameLength);
    std::int32_t GetSockName(std::uint32_t s, SockAddrIn *name, std::int32_t *nameLength);
    std::int32_t IoctlSocket(std::uint32_t s, std::uint32_t command, std::uint32_t *argument);
    // `value` is the option's 4 bytes as a little-endian word (a BOOL, an int, or a linger:
    // l_onoff in the low half, l_linger in the high half).
    std::int32_t SetSockOpt(std::uint32_t s, std::int32_t level, std::int32_t name,
                            const std::uint32_t *value, std::int32_t length);
    std::int32_t GetSockOpt(std::uint32_t s, std::int32_t level, std::int32_t name,
                            std::uint32_t *value, std::int32_t *length);
    // A NULL or empty name is this machine's own. A name the alias map holds (compared without
    // regard to case) is resolved as its target instead; the entry keeps the name asked for.
    std::optional<HostEntry> GetHostByName(const char *name);
    // The names the client points elsewhere: the host names the original carries for the
    // services that no longer exist, each mapped to the host or dotted quad of a stand-in
    // (app/ServiceHosts.h). Replaces the whole map; an empty map resolves every name as asked.
    void SetHostAliases(const std::map<std::string, std::string> &aliases);
    std::int32_t GetHostName(char *name, std::int32_t length);

private:
    struct Entry {
        platform::SocketId id;
        platform::SocketKind kind = platform::SocketKind::kUdp;
        bool connectPending = false; // a non-blocking TCP connect has not been seen to end
        std::int32_t socketError = 0; // SO_ERROR
    };

    platform::Transport &transport_;
    std::mutex mutex_;
    std::unordered_map<std::uint32_t, Entry> sockets_;
    std::uint32_t nextSocket_ = 0x100;
    std::uint32_t started_ = 0;
    std::map<std::string, std::string> aliases_; // lower-case name -> target

    bool started();
    std::optional<Entry> lookup(std::uint32_t s);
    std::uint32_t add(platform::SocketId id, platform::SocketKind kind);
    void setPending(std::uint32_t s, bool pending, std::int32_t socketError);
    // The error for a blocking call that failed: WSAEINTR if the socket was closed under it.
    std::int32_t blockedError(std::uint32_t s, platform::Error error, std::int32_t otherwise);
    void closeAll();
};

// The process's module instance, used by the C entry points of WinsockApi.h; made on first use
// on platform::makeTransport().
Winsock &guestWinsock();
// Replaces the process's instance with one on `transport` (the guest runtime's, or a test's).
void installGuestWinsock(std::unique_ptr<platform::Transport> transport);
// Sets the alias map of the process's instance, now and for any instance made later.
void setGuestHostAliases(const std::map<std::string, std::string> &aliases);

} // namespace sfcop::hle::winsock
