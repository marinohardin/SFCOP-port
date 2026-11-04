// IDirectPlay4A on the Transport component (DESIGN §4.3, §5.11, §6.1): the game's DirectPlay
// object as the methods the game calls (analysis/netcode/directplay.md), over the LAN protocol
// of Wire.h. One object is one machine in at most one session at a time.
//
// Model. A session has a host machine, which answers enumeration on the enumeration port and
// admits joiners; every machine (peer) talks to every other one directly. Each peer owns the
// players and groups it creates and tells every other peer about them; ids are the owner's peer
// number in the high 16 bits and its own counter in the low 16, so no round trip is needed to
// create one. Guaranteed messages, and every change to the name table, travel on a per-peer
// sequenced and acknowledged channel; others are single datagrams. A peer that is silent for
// Config::peerTimeoutMs is gone: its players are destroyed, and if it was the host of a
// DPSESSION_MIGRATEHOST session, the remaining peer with the lowest number becomes host.
//
// Threading. Every method may be called from any thread; one lock serialises them. Methods that
// wait (EnumSessions, Open) release the lock while they wait. Callbacks and Environment::signal
// run without the lock held, so a callback may call back into the object.
//
// Lifetime. The object mirrors a COM object's reference count (CONVENTIONS §4.2): it starts at
// one, and whoever holds it deletes it when Release() returns 0. Deleting it joins the service
// thread first, without a time limit, through Environment::blocking; the object is never freed
// while the thread can still run.
#pragma once

#include "hle/dplay/DplayDefs.h"
#include "hle/dplay/Wire.h"
#include "platform/Clock/Clock.h"
#include "platform/Threads/Threads.h"
#include "platform/Transport/Transport.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace sfcop::hle::dplay {

// The components an object works on.
struct Environment {
    platform::Transport *transport = nullptr; // required
    platform::Clock *clock = nullptr;         // required; without Ticks() nothing waits
    // Optional: when given, Open() starts a service thread that keeps the session alive while
    // the game makes no calls. Without it the object is serviced on each call only.
    platform::Threads *threads = nullptr;
    // How a method that blocks waits `milliseconds`: on the game's thread, the guest runtime
    // releases the machine around Clock::Sleep. Empty: Clock::Sleep.
    std::function<void(std::uint32_t)> wait;
    // Runs the function it is given with the game's machine released, so that a thread that
    // needs the machine (`signal` below) can run while this one waits for it. The destructor
    // joins the service thread through it. On the game's thread, the guest runtime brackets
    // the call with hle_block_begin/hle_block_end. Empty: the function runs as it is.
    std::function<void(const std::function<void()> &)> blocking;
    // Signals the event a local player was created with (CreatePlayer's hEvent), when a message
    // for that player arrives. Never called with 0.
    std::function<void(std::uint32_t)> signal;
};

// The protocol's numbers; tests shorten the times.
struct Config {
    std::uint16_t enumPort = wire::kDefaultEnumPort;
    std::uint32_t broadcastAddress = 0xFFFFFFFF;
    // Also send enumeration requests to 127.0.0.1, so that a host on this machine is found
    // when the network does not loop broadcasts back.
    bool searchLoopback = true;
    std::uint64_t seed = 0x5346434F50; // instance GUIDs; the guest bridge seeds it at random
    std::uint32_t timeoutMs = 1000;      // DPCAPS.dwTimeout, and EnumSessions' default
    std::uint32_t joinTimeoutMs = 5000;  // Open(DPOPEN_JOIN)
    std::uint32_t resendMs = 250;        // enumeration and join requests
    std::uint32_t retransmitMs = 200;    // unacknowledged channel messages
    std::uint32_t keepaliveMs = 1000;    // a peer sent nothing for this long gets a keepalive
    std::uint32_t peerTimeoutMs = 10000; // a peer heard nothing from for this long is gone
};

// Limits on what a peer, or the network, can make the object keep. Past one, the oldest item is
// dropped (or, for entities, the new one refused) and a counter below goes up.
constexpr std::size_t kMaxInbox = 4096;            // messages waiting for Receive
constexpr std::size_t kMaxFound = 256;             // sessions an enumeration remembers
constexpr std::size_t kMaxUnacked = 4096;          // unacknowledged channel messages per peer
constexpr std::size_t kMaxEntitiesPerPeer = 1024;  // players and groups one peer may own

// How often the object refused or dropped something, for tests and diagnostics.
struct Counters {
    std::uint32_t rejectedChanges = 0;  // a peer's change to an entity it does not own
    std::uint32_t rejectedEntities = 0; // an entity add past kMaxEntitiesPerPeer
    std::uint32_t droppedMessages = 0;  // inbox overflow
    std::uint32_t droppedSessions = 0;  // found-sessions overflow
    std::uint32_t droppedUnacked = 0;   // unacknowledged-queue overflow
};

// A connection EnumConnections offers.
struct Connection {
    Guid serviceProvider;
    std::vector<std::uint8_t> address;
    Name name;
    std::uint32_t flags = 0;
};

// A player or group as EnumPlayers and EnumGroups report it.
struct PlayerEntry {
    Dpid id = 0;
    std::uint32_t playerType = kPlayerTypePlayer;
    Name name;
    std::uint32_t flags = 0; // kEnumPlayersLocal or kEnumPlayersRemote, | kEnumPlayersSpectator
};

// A message Receive hands out: a game message's bytes (no pointers), or a system message in
// the guest's DPMSG_* layout.
struct ReceivedMessage {
    Dpid from = 0;
    Dpid to = 0;
    Blob blob;
};

using SessionCallback =
    std::function<bool(const SessionDesc *session, std::uint32_t &timeoutMs, std::uint32_t flags)>;
using PlayerCallback = std::function<bool(const PlayerEntry &entry)>;
using ConnectionCallback = std::function<bool(const Connection &connection)>;

class DirectPlay {
public:
    explicit DirectPlay(Environment env, Config config = {});
    ~DirectPlay();
    DirectPlay(const DirectPlay &) = delete;
    DirectPlay &operator=(const DirectPlay &) = delete;

    std::uint32_t AddRef();
    std::uint32_t Release();

    // ---- connection ---------------------------------------------------------------------
    // Reports the TCP/IP connection, whatever the application GUID. DPCONNECTION_DIRECTPLAY or
    // 0 in `flags`; DPCONNECTION_DIRECTPLAYLOBBY lists nothing.
    HResult EnumConnections(const Guid *application, const ConnectionCallback &callback,
                            std::uint32_t flags);
    // Takes an address whose service provider is TCP/IP. DPERR_ALREADYINITIALIZED the second
    // time; DPERR_UNAVAILABLE for another provider; DPERR_INVALIDPARAMS for a malformed one.
    HResult InitializeConnection(const std::uint8_t *address, std::size_t bytes,
                                 std::uint32_t flags);

    // ---- sessions -----------------------------------------------------------------------
    // Synchronous: searches for `timeoutMs` (0: Config::timeoutMs), then reports each session
    // found and calls `callback` once more with no session and DPESC_TIMEDOUT; if that call
    // returns true it searches again for the timeout the callback left. Asynchronous
    // (DPENUMSESSIONS_ASYNC): reports what has been found so far and keeps searching until
    // DPENUMSESSIONS_STOPASYNC.
    HResult EnumSessions(const SessionDesc &filter, std::uint32_t timeoutMs,
                         const SessionCallback &callback, std::uint32_t flags);
    // DPOPEN_CREATE hosts a new session from `desc`; DPOPEN_JOIN joins the enumerated session
    // whose instance GUID `desc` gives, with its password, waiting for the host's answer.
    HResult Open(const SessionDesc &desc, std::uint32_t flags);
    // Leaves the session: the other machines are told, and the local players are gone.
    HResult Close();
    HResult GetSessionDesc(SessionDesc *out);
    // The host only: name, password, flags, player limit and user words change everywhere.
    HResult SetSessionDesc(const SessionDesc &desc, std::uint32_t flags);
    HResult GetCaps(Caps *out, std::uint32_t flags);

    // ---- players and groups -------------------------------------------------------------
    HResult CreatePlayer(Dpid *out, const Name &name, std::uint32_t event,
                         const std::vector<std::uint8_t> &data, std::uint32_t flags);
    HResult DestroyPlayer(Dpid player);
    HResult CreateGroup(Dpid *out, const Name &name, const std::vector<std::uint8_t> &data,
                        std::uint32_t flags);
    HResult DestroyGroup(Dpid group);
    HResult AddPlayerToGroup(Dpid group, Dpid player);
    HResult DeletePlayerFromGroup(Dpid group, Dpid player);
    HResult EnumPlayers(const Guid *instance, const PlayerCallback &callback,
                        std::uint32_t flags);
    HResult EnumGroups(const Guid *instance, const PlayerCallback &callback,
                       std::uint32_t flags);
    HResult GetPlayerName(Dpid player, Name *out);
    HResult SetPlayerName(Dpid player, const Name &name, std::uint32_t flags);
    HResult GetPlayerData(Dpid player, std::vector<std::uint8_t> *out, std::uint32_t flags);
    HResult SetPlayerData(Dpid player, const std::vector<std::uint8_t> &data,
                          std::uint32_t flags);
    HResult GetPlayerFlags(Dpid player, std::uint32_t *out);
    // A TCP/IP address (Address.h) of the machine the player is on.
    HResult GetPlayerAddress(Dpid player, std::vector<std::uint8_t> *out);

    // ---- messages -----------------------------------------------------------------------
    HResult Send(Dpid from, Dpid to, std::uint32_t flags, const std::uint8_t *data,
                 std::size_t bytes);
    // As Send; DPSEND_ASYNC returns DPERR_PENDING with a message id. Priority and timeout are
    // accepted and not used: nothing waits in a send queue.
    HResult SendEx(Dpid from, Dpid to, std::uint32_t flags, const std::uint8_t *data,
                   std::size_t bytes, std::uint32_t priority, std::uint32_t timeoutMs,
                   std::uint32_t *messageId);
    // The first message the flags select. `*size` is the room the caller has; if the message
    // needs more, DPERR_BUFFERTOOSMALL with `*size` set and the message kept. On success `*size`
    // is the message's size and `*from`, `*to` and `out` are filled.
    HResult Receive(Dpid *from, Dpid *to, std::uint32_t flags, std::uint32_t *size,
                    ReceivedMessage *out);
    HResult GetMessageCount(Dpid player, std::uint32_t *count);
    // DPMESSAGEQUEUE_SEND: guaranteed messages not yet acknowledged; _RECEIVE: messages waiting.
    HResult GetMessageQueue(Dpid from, Dpid to, std::uint32_t flags, std::uint32_t *messages,
                            std::uint32_t *bytes);
    // Nothing waits in a send queue, so there is nothing to cancel.
    HResult CancelMessage(std::uint32_t messageId, std::uint32_t flags);

    // ---- service ------------------------------------------------------------------------
    // Waits up to `maxWaitMs` for a datagram (Transport::Select), then handles every datagram
    // waiting and every timer due. Does not wait through Environment::wait.
    void Service(std::uint32_t maxWaitMs);
    // The sockets the object has open, for a caller's own Select.
    std::vector<platform::SocketId> Sockets();
    Counters GetCounters();

private:
    // kLost: the session's host went and the session cannot migrate (DPSYS_SESSIONLOST was
    // delivered). Calls that need a session answer DPERR_SESSIONLOST until Close().
    enum class State { kCreated, kInitialized, kJoining, kInSession, kLost };

    struct Unacked {
        std::uint32_t seq = 0;
        std::vector<std::uint8_t> datagram;
        std::uint32_t payloadBytes = 0;
        std::uint32_t lastSent = 0;
    };
    struct Peer {
        std::uint32_t id = 0;
        platform::Endpoint endpoint;
        std::uint32_t lastHeard = 0;
        std::uint32_t lastSent = 0;
        std::uint32_t nextSeq = 1;
        std::deque<Unacked> unacked;
        std::uint32_t expected = 1;
        std::map<std::uint32_t, std::vector<std::uint8_t>> early;
    };
    struct Found {
        SessionDesc desc;
        platform::Endpoint host;
        std::uint64_t order = 0; // when it was first heard, for dropping the oldest
    };
    struct Queued {
        Dpid from = 0;
        Dpid to = 0;
        Blob blob;
    };

    class Lock;

    // packets (DirectPlayNet.cpp)
    void pump();
    void handleDatagram(const std::uint8_t *data, std::size_t bytes, platform::Endpoint from);
    void handleEnumRequest(const wire::Packet &p, platform::Endpoint from);
    void handleEnumReply(const wire::Packet &p, platform::Endpoint from);
    void handleJoinRequest(const wire::Packet &p, platform::Endpoint from);
    void handleReliable(Peer &peer, const wire::Packet &p);
    void handleInner(Peer &peer, const wire::Inner &m);
    void handleJoinAccept(Peer &host, const wire::Inner &m);
    void timers();
    void dropPeer(std::uint32_t id);
    void becomeHost();
    void sendTo(platform::Endpoint to, const std::vector<std::uint8_t> &datagram);
    void sendReliable(Peer &peer, const std::vector<std::uint8_t> &inner,
                      std::uint32_t payloadBytes = 0);
    void broadcastReliable(const std::vector<std::uint8_t> &inner);
    void sendEnumRequests();
    Peer &addPeer(std::uint32_t id, platform::Endpoint endpoint);
    Peer *peerOf(std::uint32_t id);

    // name table and messages (DirectPlay.cpp)
    void addEntity(const wire::Entity &entity, bool announce);
    void removeEntity(Dpid id, bool announce);
    void deliverData(Dpid from, Dpid to, const std::uint8_t *data, std::size_t bytes);
    void deliverSystem(const Blob &message, Dpid except);
    void queueFor(Dpid player, Dpid from, Blob blob);
    std::uint32_t playerCount() const;
    bool isLocal(Dpid id) const;
    std::vector<Dpid> localPlayers() const;
    SessionDesc currentDesc() const;
    std::vector<wire::Entity> ownEntities() const;
    HResult notInSession() const;
    bool ownsChange(const Peer &sender, Dpid entity);
    HResult openSockets(bool host);
    void closeSession();
    void startService();
    std::uint32_t now();
    bool haveClock();
    void waitFor(std::uint32_t milliseconds);
    Guid newGuid();
    Dpid newId();

    Environment env_;
    Config config_;
    std::mutex mutex_;
    std::vector<std::uint32_t> toSignal_; // events to signal once the lock is released
    std::atomic<std::uint32_t> refs_{1};

    State state_ = State::kCreated;
    platform::Endpoint target_; // enumeration target; address 0 = search the network
    bool haveTarget_ = false;
    platform::SocketId game_{};
    platform::SocketId enum_{};

    // enumeration
    std::uint32_t enumRequest_ = 0;
    Guid enumApplication_;
    bool enumerating_ = false;
    bool asyncEnum_ = false;
    std::uint32_t lastEnumSent_ = 0;
    std::map<Guid, Found> found_;
    std::uint64_t foundOrder_ = 0;

    // joining
    Guid joinInstance_;
    platform::Endpoint joinHost_;
    std::optional<std::string> joinPassword_;
    HResult joinResult_ = kDpOk;
    bool joinAnswered_ = false;
    std::uint32_t lastJoinSent_ = 0;

    // the session
    SessionDesc desc_;
    bool isHost_ = false;
    std::uint32_t key_ = 0;
    std::uint32_t self_ = 0;     // this machine's peer number
    std::uint32_t host_ = 0;     // the host's peer number; 0 when the session has none
    std::uint32_t nextPeer_ = 0; // the next peer number the host gives
    std::uint16_t nextLocal_ = 1;
    std::map<std::uint32_t, Peer> peers_;
    std::map<Dpid, wire::Entity> entities_;
    std::map<Dpid, std::vector<std::uint8_t>> localData_;
    std::map<Dpid, std::uint32_t> events_; // local player -> its event
    std::deque<Queued> inbox_;
    std::uint32_t nextMessageId_ = 1;
    Counters counters_;

    std::vector<std::uint8_t> rx_; // one datagram
    std::uint64_t random_;
    std::optional<platform::Waitable> service_;
    std::atomic<bool> stopService_{false};
    std::atomic<bool> serviceStarted_{false}; // the service thread's body has begun
    std::atomic<bool> serviceExited_{false};  // ...and touches this object no more
};

} // namespace sfcop::hle::dplay
