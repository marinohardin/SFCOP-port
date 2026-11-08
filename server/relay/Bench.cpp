// Measures the relay's forwarding rate with the limits lifted and no socket in the way: how many
// envelopes per second the state machine can pass. `sfcop-relay-bench [datagrams] [payload]`.
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "RelayService.h"
#include "Sha256.h"
#include "Wire.h"

using namespace sfcop::relay;

namespace {

class CountingSender : public Sender {
public:
    void send(const Endpoint&, const std::uint8_t*, std::size_t size) override {
        ++datagrams;
        bytes += size;
    }
    std::uint64_t datagrams = 0, bytes = 0;
};

class FixedTime : public TimeSource {
public:
    std::uint64_t nowMs() const override { return 1000; }
    std::uint32_t unixSeconds() const override { return 1800000000; }
};

}  // namespace

int main(int argc, char** argv) {
    std::size_t count = argc > 1 ? std::size_t(std::atoll(argv[1])) : 2000000;
    std::size_t payload = argc > 2 ? std::size_t(std::atoll(argv[2])) : 200;

    CountingSender out;
    FixedTime time;
    Logger log(LogLevel::kError, nullptr);
    RelayConfig config;
    config.ticketKey = Bytes(32, 1);
    config.relayDatagramsPerSecond = 0x7fffffff;
    config.relayBytesPerSecond = 0x7fffffff;
    config.bindsPerMinute = config.rendezvousPerMinute = 1000;
    RelayService service(config, out, time, log, [] { return 1u; });

    // Bind two peers with tickets made the same way the lobby makes them.
    for (std::uint32_t peer = 1; peer <= 2; ++peer) {
        Ticket t;
        t.sessionId = 1;
        t.peerId = peer;
        t.expiresAt = 1800000600;
        t.role = peer == 1 ? 1 : 2;
        std::uint8_t body[20];
        encodeTicketBody(t, body);
        Digest d = hmacSha256(config.ticketKey.data(), config.ticketKey.size(), body, sizeof body);
        for (int i = 0; i < 16; ++i) t.mac[i] = d[std::size_t(i)];
        BindMessage m;
        m.header.transaction = 1;
        m.ticket = t;
        Bytes b = encodeBind(m);
        service.onDatagram({0xC0000200u + peer, std::uint16_t(40000 + peer)}, b.data(), b.size());
    }

    out.datagrams = 0;

    Bytes env(32 + payload, 0xAB);
    for (int i = 4; i < 32; ++i) env[std::size_t(i)] = 0;
    env[0] = 0x53; env[1] = 0x46; env[2] = 0x4E; env[3] = 0x45;
    env[4] = 1; env[5] = 1;
    env[8] = 1;
    env[16] = 1;
    env[20] = 2;
    env[30] = 1;

    auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < count; ++i)
        service.onDatagram({0xC0000201u, 40001}, env.data(), env.size());
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::printf("forwarded %llu of %zu datagrams of %zu bytes in %.3f s: %.0f datagrams/s, %.1f MB/s\n",
                static_cast<unsigned long long>(out.datagrams), count, env.size(), seconds,
                double(out.datagrams) / seconds, double(out.bytes) / seconds / 1e6);
    return out.datagrams == count ? 0 : 1;
}
