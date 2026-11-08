#include "net/PeerLink.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

namespace sfcop::net {
namespace {

using Bytes = std::vector<std::uint8_t>;
using platform::Error;

constexpr std::uint64_t kSession = 0x5346434F50000001ull;

// Hands every envelope in `out` to `to`, on the route each was sent on (kChosen counts as
// direct), and clears `out`. Returns how many `to` took.
int pass(std::vector<OutgoingEnvelope> &out, PeerLink &to, std::uint32_t now,
         std::vector<OutgoingEnvelope> &answers) {
    int taken = 0;
    for (const OutgoingEnvelope &sent : out) {
        std::optional<Envelope> envelope = decodeEnvelope(sent.bytes.data(), sent.bytes.size());
        REQUIRE(envelope);
        Route route = sent.route == Route::kRelay ? Route::kRelay : Route::kDirect;
        taken += to.Receive(*envelope, route, now, answers) ? 1 : 0;
    }
    out.clear();
    return taken;
}

Bytes pattern(std::size_t bytes) {
    Bytes out(bytes);
    for (std::size_t i = 0; i < bytes; ++i) {
        out[i] = static_cast<std::uint8_t>(i * 7 + 3);
    }
    return out;
}

std::optional<Envelope> decodeOut(const OutgoingEnvelope &out) {
    return decodeEnvelope(out.bytes.data(), out.bytes.size());
}

} // namespace

TEST_CASE("a message over 1168 bytes travels as fragments and arrives whole, in any order") {
    PeerLink a(kSession, 2, 1, 0);
    PeerLink b(kSession, 1, 2, 0);
    std::vector<OutgoingEnvelope> out, answers;
    const Bytes message = pattern(3000);
    REQUIRE(a.Send(message.data(), message.size(), false, 10, out).Ok());
    REQUIRE(out.size() == 3); // 1168 + 1168 + 664
    std::optional<Envelope> first = decodeOut(out[0]);
    std::optional<Envelope> last = decodeOut(out[2]);
    REQUIRE(first);
    REQUIRE(last);
    CHECK(first->header.fragmentCount == 3);
    CHECK(first->header.flags == kEnvelopeFragment);
    CHECK(first->payload.size() == 1168);
    CHECK(last->header.fragmentIndex == 2);
    CHECK(last->payload.size() == 664);
    CHECK(first->header.sequence == last->header.sequence);

    std::reverse(out.begin(), out.end());
    CHECK(pass(out, b, 20, answers) == 3);
    REQUIRE(b.HasMessage());
    CHECK(b.NextMessageBytes() == 3000);
    CHECK(b.TakeMessage() == message);
    CHECK_FALSE(b.HasMessage());
}

TEST_CASE("an unreliable envelope seen before, or older than the 1024-number window, is dropped") {
    PeerLink a(kSession, 2, 1, 0);
    PeerLink b(kSession, 1, 2, 0);
    std::vector<OutgoingEnvelope> out, answers;
    const Bytes one = {0x01};
    REQUIRE(a.Send(one.data(), 1, false, 0, out).Ok());
    std::vector<OutgoingEnvelope> copy = out;
    CHECK(pass(out, b, 0, answers) == 1);
    CHECK(pass(copy, b, 0, answers) == 0); // the duplicate
    CHECK(b.TakeMessage() == one);
    CHECK_FALSE(b.HasMessage());

    // Sequence 1 was taken; 1100 more move the window past it.
    Envelope late;
    late.header.kind = EnvelopeKind::kData;
    late.header.sessionId = kSession;
    late.header.source = 2;
    late.header.destination = 1;
    late.header.sequence = 1101;
    late.payload = {0x02};
    CHECK(b.Receive(late, Route::kDirect, 0, answers));
    late.header.sequence = 50; // 1051 behind: outside the window
    CHECK_FALSE(b.Receive(late, Route::kDirect, 0, answers));
    late.header.sequence = 1000; // inside the window, not seen before
    CHECK(b.Receive(late, Route::kDirect, 0, answers));
}

TEST_CASE("an envelope for another session, destination or source is ignored") {
    PeerLink b(kSession, 1, 2, 0);
    std::vector<OutgoingEnvelope> answers;
    Envelope envelope;
    envelope.header.kind = EnvelopeKind::kData;
    envelope.header.sessionId = kSession + 1;
    envelope.header.source = 2;
    envelope.header.destination = 1;
    envelope.header.sequence = 1;
    envelope.payload = {0x01};
    CHECK_FALSE(b.Receive(envelope, Route::kDirect, 0, answers));
    envelope.header.sessionId = kSession;
    envelope.header.destination = 3;
    CHECK_FALSE(b.Receive(envelope, Route::kDirect, 0, answers));
    envelope.header.destination = 1;
    envelope.header.source = 4;
    CHECK_FALSE(b.Receive(envelope, Route::kDirect, 0, answers));
    CHECK_FALSE(b.HasMessage());
}

TEST_CASE("a ping is answered on the path it came in on, and its pong is seen there") {
    PeerLink a(kSession, 2, 1, 0);
    PeerLink b(kSession, 1, 2, 0);
    std::vector<OutgoingEnvelope> out, answers, back;
    std::uint32_t nonce = a.Ping(Route::kRelay, 100, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].route == Route::kRelay);
    CHECK(pass(out, b, 130, answers) == 1);
    REQUIRE(answers.size() == 1);
    CHECK(answers[0].route == Route::kRelay);
    std::optional<Envelope> pong = decodeOut(answers[0]);
    REQUIRE(pong);
    CHECK(pong->header.kind == EnvelopeKind::kPong);
    CHECK(pong->nonce == nonce);
    CHECK(pong->sentMs == 100);
    CHECK_FALSE(a.PongSeen(Route::kRelay));
    CHECK(pass(answers, a, 160, back) == 1);
    CHECK(a.PongSeen(Route::kRelay));
    CHECK_FALSE(a.PongSeen(Route::kDirect));
    CHECK(a.SmoothedRttMs() == 60);
}

TEST_CASE("a link pings after 1 s with nothing sent and is dead after 10 s with nothing received") {
    PeerLink a(kSession, 2, 1, 0);
    std::vector<OutgoingEnvelope> out;
    a.Tick(999, out);
    CHECK(out.empty());
    a.Tick(1000, out);
    REQUIRE(out.size() == 1);
    std::optional<Envelope> ping = decodeOut(out[0]);
    REQUIRE(ping);
    CHECK(ping->header.kind == EnvelopeKind::kPing);
    CHECK(out[0].route == Route::kChosen);
    CHECK_FALSE(a.Dead(9999));
    CHECK(a.Dead(10000));
    const Bytes one = {0x01};
    out.clear();
    CHECK(a.Send(one.data(), 1, false, 10000, out).Code() == Error::kConnectionReset);
}

TEST_CASE("EnvelopeClose is retransmitted until acknowledged, then the close is finished") {
    PeerLink a(kSession, 2, 1, 0);
    PeerLink b(kSession, 1, 2, 0);
    std::vector<OutgoingEnvelope> out, answers, back;
    a.Close(0, 0, out);
    REQUIRE(out.size() == 1);
    std::optional<Envelope> close = decodeOut(out[0]);
    REQUIRE(close);
    CHECK(close->header.kind == EnvelopeKind::kClose);
    CHECK(close->header.flags == kEnvelopeReliable);
    CHECK(close->header.sequence == 1);
    out.clear(); // lost

    a.Tick(199, out);
    CHECK(out.empty());
    a.Tick(200, out); // the first timeout: 200 ms
    REQUIRE(out.size() == 1);
    CHECK(pass(out, b, 210, answers) == 1);
    CHECK(b.PeerClosed());
    CHECK(answers.empty()); // in order: the ack waits up to 50 ms
    b.Tick(259, answers);
    CHECK(answers.empty());
    b.Tick(260, answers);
    REQUIRE(answers.size() == 1);
    std::optional<Envelope> ack = decodeOut(answers[0]);
    REQUIRE(ack);
    CHECK(ack->header.kind == EnvelopeKind::kAck);
    CHECK(ack->cumulative == 1);
    CHECK_FALSE(a.CloseFinished(260));
    pass(answers, a, 270, back);
    CHECK(a.CloseFinished(270));
    const Bytes one = {0x01};
    CHECK(a.Send(one.data(), 1, false, 270, out).Code() == Error::kConnectionReset);
}

TEST_CASE("reliable messages are delivered in order and an out-of-order one is acked at once") {
    PeerLink a(kSession, 2, 1, 0);
    PeerLink b(kSession, 1, 2, 0);
    std::vector<OutgoingEnvelope> first, second, answers;
    const Bytes one = {0x01}, two = {0x02};
    REQUIRE(a.Send(one.data(), 1, true, 0, first).Ok());
    REQUIRE(a.Send(two.data(), 1, true, 0, second).Ok());
    CHECK(pass(second, b, 5, answers) == 1);
    CHECK_FALSE(b.HasMessage());
    REQUIRE(answers.size() == 1);
    std::optional<Envelope> ack = decodeOut(answers[0]);
    REQUIRE(ack);
    CHECK(ack->cumulative == 0);
    CHECK(ack->mask == 0x2); // sequence 2 held: bit 1 (cumulative + 1 + 1)
    answers.clear();
    CHECK(pass(first, b, 6, answers) == 1);
    REQUIRE(b.HasMessage());
    CHECK(b.TakeMessage() == one);
    CHECK(b.TakeMessage() == two);
}

TEST_CASE("Send refuses an empty message, one over 65507 bytes, and too many unacknowledged") {
    PeerLink a(kSession, 2, 1, 0);
    std::vector<OutgoingEnvelope> out;
    const Bytes big(65508, 0);
    CHECK(a.Send(big.data(), 0, false, 0, out).Code() == Error::kInvalidArgument);
    CHECK(a.Send(big.data(), big.size(), false, 0, out).Code() == Error::kInvalidArgument);
    CHECK(a.Send(big.data(), 65507, false, 0, out).Ok());
    CHECK(out.size() == 57);
    out.clear();
    for (int i = 0; i < 256; ++i) {
        REQUIRE(a.Send(big.data(), 1, true, 0, out).Ok());
    }
    CHECK(a.Send(big.data(), 1, true, 0, out).Code() == Error::kWouldBlock);
}

TEST_CASE("reassembly holds at most 16 messages at once") {
    PeerLink a(kSession, 2, 1, 0);
    PeerLink b(kSession, 1, 2, 0);
    std::vector<OutgoingEnvelope> answers;
    std::vector<std::vector<OutgoingEnvelope>> messages(17);
    for (std::vector<OutgoingEnvelope> &out : messages) {
        const Bytes message = pattern(3000);
        REQUIRE(a.Send(message.data(), message.size(), false, 0, out).Ok());
        REQUIRE(out.size() == 3);
    }
    for (std::vector<OutgoingEnvelope> &out : messages) {
        std::vector<OutgoingEnvelope> first(out.begin(), out.begin() + 1);
        out.erase(out.begin());
        pass(first, b, 1, answers);
    }
    pass(messages[16], b, 2, answers); // the 17th found no room: its first piece is gone
    CHECK_FALSE(b.HasMessage());
    pass(messages[0], b, 3, answers);
    REQUIRE(b.HasMessage());
    CHECK(b.TakeMessage() == pattern(3000));
    CHECK_FALSE(b.HasMessage());
}

TEST_CASE("reassembly holds at most 256 KB of pieces at once") {
    PeerLink a(kSession, 2, 1, 0);
    PeerLink b(kSession, 1, 2, 0);
    std::vector<OutgoingEnvelope> answers;
    // 60000 bytes: 51 pieces of 1168 and one of 432. Four messages less their last piece hold
    // 238272 bytes; the fifth fits 20 more pieces under 262144 and loses its 21st.
    std::vector<std::vector<OutgoingEnvelope>> messages(5);
    std::vector<std::vector<OutgoingEnvelope>> lasts(5);
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const Bytes message = pattern(60000);
        REQUIRE(a.Send(message.data(), message.size(), false, 0, messages[i]).Ok());
        REQUIRE(messages[i].size() == 52);
        lasts[i].push_back(messages[i].back());
        messages[i].pop_back();
        pass(messages[i], b, 1, answers);
    }
    CHECK_FALSE(b.HasMessage());
    pass(lasts[0], b, 2, answers); // completes the first and frees its room
    REQUIRE(b.HasMessage());
    CHECK(b.TakeMessage().size() == 60000);
    pass(lasts[4], b, 3, answers); // the fifth lost a piece: it never completes
    CHECK_FALSE(b.HasMessage());
    pass(lasts[1], b, 4, answers);
    CHECK(b.HasMessage());
}

TEST_CASE("reassembly gives a message up 5 s after its first piece") {
    PeerLink a(kSession, 2, 1, 0);
    PeerLink b(kSession, 1, 2, 0);
    std::vector<OutgoingEnvelope> out, answers, ticked;
    const Bytes message = pattern(3000);
    REQUIRE(a.Send(message.data(), message.size(), false, 0, out).Ok());
    std::vector<OutgoingEnvelope> last(out.end() - 1, out.end());
    out.pop_back();
    pass(out, b, 100, answers);
    b.Tick(5099, ticked);
    pass(last, b, 5099, answers); // still in time
    CHECK(b.HasMessage());

    REQUIRE(a.Send(message.data(), message.size(), false, 6000, out).Ok());
    last.assign(out.end() - 1, out.end());
    out.pop_back();
    pass(out, b, 6000, answers);
    b.Tick(11000, ticked);
    pass(last, b, 11000, answers); // given up: the last piece alone completes nothing
    CHECK(b.TakeMessage() == message);
    CHECK_FALSE(b.HasMessage());
}

TEST_CASE("reliable messages held out of order are capped at 256 KB until the gap closes") {
    PeerLink a(kSession, 2, 1, 0);
    PeerLink b(kSession, 1, 2, 0);
    std::vector<OutgoingEnvelope> answers;
    std::vector<std::vector<OutgoingEnvelope>> messages(6);
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const Bytes message(60000, static_cast<std::uint8_t>(i + 1));
        REQUIRE(a.Send(message.data(), message.size(), true, 0, messages[i]).Ok());
    }
    // 2 to 5 are held (240000 bytes); 6 would pass 262144 and is dropped unacknowledged.
    for (std::size_t i = 1; i < messages.size(); ++i) {
        pass(messages[i], b, 10, answers);
    }
    CHECK_FALSE(b.HasMessage());
    pass(messages[0], b, 20, answers);
    for (std::uint8_t i = 1; i <= 5; ++i) {
        REQUIRE(b.HasMessage());
        CHECK(b.TakeMessage() == Bytes(60000, i));
    }
    CHECK_FALSE(b.HasMessage());

    std::vector<OutgoingEnvelope> resent;
    a.Tick(1000, resent); // past the retransmission timeout: the unacknowledged come again
    pass(resent, b, 1000, answers);
    REQUIRE(b.HasMessage());
    CHECK(b.TakeMessage() == Bytes(60000, 6));
}

} // namespace sfcop::net
