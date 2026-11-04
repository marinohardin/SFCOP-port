// Tests only: a DirectPlay object on TransportMock, and the hand-written datagrams a host at
// 192.0.2.10:2300 sends it, so that a test can bring it into a session without a second object.
#pragma once

#include "hle/dplay/DplayTestUtil.h"
#include "platform/Transport/TransportMock.h"

#include <doctest/doctest.h>

namespace sfcop::hle::dplay::test {

inline const platform::Endpoint kHostEndpoint{0xC000020A, 2300};

// The session the mock host offers: {A1A2A3A4-B1B2-C1C2-D1D2-D3D4D5D6D7D8}.
inline Guid mockInstance() {
    return makeGuid(0xA1A2A3A4, 0xB1B2, 0xC1C2, {0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8});
}

// The session key the mock host's peer packets carry.
inline const std::vector<std::uint8_t> kMockKey = hex("0D F0 AD 0B");

// The host's reply to enumeration request `request`: protocol, keepalive and migrate-host
// flags (0x2044), 8 players at most and 1 now, user words 1 to 4, named "TestGame".
inline std::vector<std::uint8_t> mockEnumReply(std::uint8_t request) {
    return concat({hex("53 46 44 50 01 02 00 00"), {request, 0, 0, 0}, hex("44 20 00 00"),
                   guidBytes(mockInstance()), guidBytes(testApplication()),
                   hex("08 00 00 00 01 00 00 00"),
                   hex("01 00 00 00 02 00 00 00 03 00 00 00 04 00 00 00"),
                   hex("08 00 54 65 73 74 47 61 6D 65 FF FF")});
}

// The host's join accept: the joiner is peer 2, the next peer is 3, no other peers, and one
// player, the host's 0x00010001 "TestCaptain" with remote data DE AD BE EF.
inline std::vector<std::uint8_t> mockJoinAccept() {
    return concat({hex("53 46 44 50 01 05 00 00"), kMockKey, hex("01 00 00 00 01 00 00 00"),
                   hex("01 02 00 00 00 03 00 00 00"), hex("44 20 00 00"),
                   guidBytes(mockInstance()), guidBytes(testApplication()),
                   hex("08 00 00 00 01 00 00 00"),
                   hex("01 00 00 00 02 00 00 00 03 00 00 00 04 00 00 00"),
                   hex("08 00 54 65 73 74 47 61 6D 65 FF FF"), hex("00 00 01 00"),
                   hex("01 00 01 00 00 00 00 00 00 01 00 00 00"),
                   hex("0B 00 54 65 73 74 43 61 70 74 61 69 6E FF FF"),
                   hex("04 00 00 00 DE AD BE EF 00 00")});
}

// A channel message from the host, after the join accept: `seq` and the inner message.
inline std::vector<std::uint8_t> mockReliable(std::uint8_t seq,
                                              const std::vector<std::uint8_t> &inner) {
    return concat({hex("53 46 44 50 01 05 00 00"), kMockKey, hex("01 00 00 00"),
                   {seq, 0, 0, 0}, inner});
}

// A DirectPlay object on a TransportMock that has joined the mock host's session as peer 2.
struct MockJoined {
    platform::TransportMock transport;
    FakeClock clock;
    std::vector<std::uint32_t> signalled;
    std::unique_ptr<DirectPlay> dp;

    // `adjust`, if given, may change the Environment before the object is made.
    explicit MockJoined(Config config = Lan::testConfig(),
                        const std::function<void(Environment &)> &adjust = {}) {
        Environment env;
        env.transport = &transport;
        env.clock = &clock;
        env.signal = [this](std::uint32_t event) { signalled.push_back(event); };
        if (adjust) {
            adjust(env);
        }
        dp = std::make_unique<DirectPlay>(env, config);
        REQUIRE(initialize(*dp) == kDpOk);
        transport.incoming.push_back({mockEnumReply(1), kHostEndpoint});
        SessionDesc filter;
        REQUIRE(dp->EnumSessions(
                    filter, 50,
                    [](const SessionDesc *, std::uint32_t &, std::uint32_t) { return false; },
                    0) == kDpOk);
        transport.incoming.push_back({mockJoinAccept(), kHostEndpoint});
        SessionDesc d;
        d.instance = mockInstance();
        REQUIRE(dp->Open(d, kOpenJoin) == kDpOk);
        transport.sent.clear();
        transport.log.Clear();
    }
};

} // namespace sfcop::hle::dplay::test
