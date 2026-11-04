// The LAN protocol between native clients (README.md, "Wire format"): UDP datagrams, one
// protocol message each, little-endian. This project's own format; it does not interoperate
// with DirectPlay, and Phase 3 replaces the transport under it (DESIGN §5.11, §6.7). Every
// decoder checks bounds and refuses a malformed datagram as a whole.
#pragma once

#include "hle/dplay/DplayDefs.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace sfcop::hle::dplay::wire {

constexpr std::uint32_t kMagic = 0x50444653; // "SFDP" as bytes
constexpr std::uint8_t kVersion = 1;
// The port a host listens on for session enumeration.
constexpr std::uint16_t kDefaultEnumPort = 47700;
// The largest datagram sent: the largest UDP payload over IPv4.
constexpr std::size_t kMaxDatagram = 65507;
// The largest game message Send carries (dwMaxBufferSize in GetCaps).
constexpr std::uint32_t kMaxPayload = 60000;

enum class PacketType : std::uint8_t {
    kEnumRequest = 1, // anyone -> host enumeration port
    kEnumReply = 2,   // host -> enumerator
    kJoinRequest = 3, // joiner -> host
    kJoinReject = 4,  // host -> joiner
    kReliable = 5,    // peer -> peer, sequenced and acknowledged
    kAck = 6,         // peer -> peer
    kUnreliable = 7,  // peer -> peer, one game message, not acknowledged
    kKeepalive = 8,   // peer -> peer
    kLeave = 9,       // peer -> peer, the sender closed the session
};

enum class InnerType : std::uint8_t {
    kJoinAccept = 1,
    kPeerAdd = 2,
    kPeerRemove = 3,
    kEntityAdd = 4,
    kEntityRemove = 5,
    kSetData = 6,
    kSetName = 7,
    kMemberAdd = 8,
    kMemberRemove = 9,
    kSessionDesc = 10,
    kData = 11,
};

// A machine in the session. `address` 0 means "the address you reach the host at".
struct PeerInfo {
    std::uint32_t id = 0;
    std::uint32_t address = 0;
    std::uint16_t port = 0;

    friend bool operator==(const PeerInfo &a, const PeerInfo &b) {
        return a.id == b.id && a.address == b.address && a.port == b.port;
    }
};

// A player or group as the name table holds it.
struct Entity {
    Dpid id = 0;
    bool group = false;
    std::uint32_t flags = 0;
    std::uint32_t owner = 0; // the peer that created it
    Name name;
    std::vector<std::uint8_t> data; // the remote data every machine sees
    std::vector<Dpid> members;      // groups only

    friend bool operator==(const Entity &a, const Entity &b) {
        return a.id == b.id && a.group == b.group && a.flags == b.flags && a.owner == b.owner &&
               a.name == b.name && a.data == b.data && a.members == b.members;
    }
};

// A decoded datagram. Only the fields of its type are set.
struct Packet {
    PacketType type = PacketType::kKeepalive;
    std::uint32_t sessionKey = 0; // peer packets
    std::uint32_t sender = 0;     // peer packets: the sender's peer id
    std::uint32_t requestId = 0;  // enum request and reply
    Guid id;                      // enum request: application; join: instance
    SessionDesc desc;             // enum reply
    std::optional<std::string> password; // join request
    HResult result = 0;                  // join reject
    std::uint32_t seq = 0;               // reliable: sequence; ack: cumulative
    Dpid from = 0, to = 0;               // unreliable
    std::vector<std::uint8_t> body;      // reliable: the inner message; unreliable: payload
};

// A decoded inner (reliable) message. Only the fields of its type are set.
struct Inner {
    InnerType type = InnerType::kData;
    std::uint32_t yourPeer = 0;     // join accept
    std::uint32_t nextPeer = 0;     // join accept
    SessionDesc desc;               // join accept, session desc
    std::vector<PeerInfo> peers;    // join accept: every peer but the host and the joiner
    std::vector<Entity> entities;   // join accept; entity add (one)
    PeerInfo peer;                  // peer add
    std::uint32_t id = 0;           // peer remove, entity remove, set data, set name
    std::vector<std::uint8_t> data; // set data; data: payload
    Name name;                      // set name
    Dpid group = 0, player = 0;     // member add, member remove
    Dpid from = 0, to = 0;          // data
};

// ---- datagrams ----------------------------------------------------------------------------

std::vector<std::uint8_t> encodeEnumRequest(std::uint32_t requestId, const Guid &application);
// The session as an enumerator sees it: without its password, with kSessionPasswordRequired
// set when it has one.
std::vector<std::uint8_t> encodeEnumReply(std::uint32_t requestId, const SessionDesc &desc);
std::vector<std::uint8_t> encodeJoinRequest(const Guid &instance,
                                            const std::optional<std::string> &password);
std::vector<std::uint8_t> encodeJoinReject(const Guid &instance, HResult result);
std::vector<std::uint8_t> encodeReliable(std::uint32_t key, std::uint32_t sender,
                                         std::uint32_t seq,
                                         const std::vector<std::uint8_t> &inner);
std::vector<std::uint8_t> encodeAck(std::uint32_t key, std::uint32_t sender,
                                    std::uint32_t cumulative);
std::vector<std::uint8_t> encodeUnreliable(std::uint32_t key, std::uint32_t sender, Dpid from,
                                           Dpid to, const std::uint8_t *payload,
                                           std::size_t bytes);
std::vector<std::uint8_t> encodeKeepalive(std::uint32_t key, std::uint32_t sender);
std::vector<std::uint8_t> encodeLeave(std::uint32_t key, std::uint32_t sender);

std::optional<Packet> decodePacket(const std::uint8_t *data, std::size_t bytes);

// ---- inner messages -----------------------------------------------------------------------

std::vector<std::uint8_t> innerJoinAccept(std::uint32_t yourPeer, std::uint32_t nextPeer,
                                          const SessionDesc &desc,
                                          const std::vector<PeerInfo> &peers,
                                          const std::vector<Entity> &entities);
std::vector<std::uint8_t> innerPeerAdd(const PeerInfo &peer);
std::vector<std::uint8_t> innerPeerRemove(std::uint32_t peer);
std::vector<std::uint8_t> innerEntityAdd(const Entity &entity);
std::vector<std::uint8_t> innerEntityRemove(Dpid id);
std::vector<std::uint8_t> innerSetData(Dpid id, const std::vector<std::uint8_t> &data);
std::vector<std::uint8_t> innerSetName(Dpid id, const Name &name);
std::vector<std::uint8_t> innerMember(bool add, Dpid group, Dpid player);
std::vector<std::uint8_t> innerSessionDesc(const SessionDesc &desc);
std::vector<std::uint8_t> innerData(Dpid from, Dpid to, const std::uint8_t *payload,
                                    std::size_t bytes);

std::optional<Inner> decodeInner(const std::vector<std::uint8_t> &body);

} // namespace sfcop::hle::dplay::wire
