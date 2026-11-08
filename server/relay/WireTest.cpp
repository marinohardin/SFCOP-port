#include <doctest/doctest.h>

#include "Wire.h"

using namespace sfcop::relay;

TEST_CASE("a Connect decodes from hand-written bytes") {
    // SFRV v1 type 3, transaction 5, session 5346434f50000001, source 2, target 1.
    const Bytes b = {0x53, 0x46, 0x52, 0x56, 1, 3, 0, 0, 5, 0, 0, 0, 0x01, 0x00, 0x00, 0x50,
                     0x4f, 0x43, 0x46, 0x53, 2, 0, 0, 0, 1, 0, 0, 0};
    ConnectMessage m;
    REQUIRE(decodeConnect(b.data(), b.size(), m));
    CHECK(m.header.transaction == 5);
    CHECK(m.sessionId == 0x5346434f50000001ull);
    CHECK(m.source == 2);
    CHECK(m.target == 1);
    CHECK(encodeConnect(m) == b);
}

TEST_CASE("a Connect with a trailing byte or too short is refused") {
    Bytes b = {0x53, 0x46, 0x52, 0x56, 1, 3, 0, 0, 5, 0, 0, 0, 0x01, 0x00, 0x00, 0x50,
               0x4f, 0x43, 0x46, 0x53, 2, 0, 0, 0, 1, 0, 0, 0};
    ConnectMessage m;
    Bytes longer = b;
    longer.push_back(0);
    CHECK_FALSE(decodeConnect(longer.data(), longer.size(), m));
    CHECK_FALSE(decodeConnect(b.data(), b.size() - 1, m));
}

TEST_CASE("a Bind decodes its ticket and candidates and refuses a count over eight") {
    BindMessage in;
    in.header.transaction = 9;
    in.ticket.sessionId = 0x1122334455667788ull;
    in.ticket.peerId = 3;
    in.ticket.expiresAt = 0x01020304;
    in.ticket.role = 2;
    in.candidates = {{0xC0000201, 4000, kLocal}};
    Bytes b = encodeBind(in);
    REQUIRE(b.size() == 58);
    CHECK(b[48] == 1);  // candidateCount
    CHECK(b[50] == 0x01);  // 192.0.2.1 on the wire is 01 02 00 C0
    CHECK(b[51] == 0x02);
    CHECK(b[52] == 0x00);
    CHECK(b[53] == 0xC0);
    BindMessage out;
    REQUIRE(decodeBind(b.data(), b.size(), out));
    CHECK(out.ticket.sessionId == 0x1122334455667788ull);
    CHECK(out.ticket.expiresAt == 0x01020304);
    REQUIRE(out.candidates.size() == 1);
    CHECK(out.candidates[0].port == 4000);

    Bytes nine(50 + 9 * 8, 0);
    std::copy(b.begin(), b.begin() + 50, nine.begin());
    nine[48] = 9;
    CHECK_FALSE(decodeBind(nine.data(), nine.size(), out));
    // A count that disagrees with the size is refused too.
    b[48] = 2;
    CHECK_FALSE(decodeBind(b.data(), b.size(), out));
}

TEST_CASE("the envelope header is read from 32 bytes and nothing else") {
    Bytes b(32, 0);
    b[0] = 0x53; b[1] = 0x46; b[2] = 0x4E; b[3] = 0x45;
    b[4] = 1; b[5] = 1;
    b[8] = 0x01; b[11] = 0x50;
    b[16] = 2; b[20] = 1;
    EnvelopeHeader h;
    REQUIRE(decodeEnvelopeHeader(b.data(), b.size(), h));
    CHECK(h.sessionId == 0x50000001ull);
    CHECK(h.source == 2);
    CHECK(h.destination == 1);
    CHECK_FALSE(decodeEnvelopeHeader(b.data(), 31, h));
}

TEST_CASE("replies encode to the sizes and bytes the document gives") {
    Bytes e = encodeRvError(7, kBadTicket);
    const Bytes expected = {0x53, 0x46, 0x52, 0x56, 1, 8, 0, 0, 7, 0, 0, 0, 1, 0, 0, 0};
    CHECK(e == expected);
    CHECK(encodeBindReply(1, 0, {1, 2, kReflexive}, 15).size() == 28);
    CHECK(encodePeerCandidates(0, 1, 2, 3, {{1, 2, kReflexive}}).size() == 40);
}
