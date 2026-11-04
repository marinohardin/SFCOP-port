#include "hle/dplay/DplayRuntime.h"

#include "hle/dplay/DplayMockFixture.h"
#include "platform/Threads/ThreadsMock.h"

#include <doctest/doctest.h>

namespace sfcop::hle::dplay {

using namespace test;

TEST_CASE("setPlatform: objects made from now on get these components") {
    platform::TransportMock transport;
    FakeClock clock;
    Platform p;
    p.transport = &transport;
    p.clock = &clock;
    setPlatform(p);
    CHECK(currentPlatform().transport == &transport);
    CHECK(currentPlatform().clock == &clock);
    setPlatform(Platform{});
    CHECK(currentPlatform().transport == nullptr);
}

TEST_CASE("a player's event: through Threads when the handle names one, else the fallback") {
    platform::ThreadsMock threads;
    Platform p;
    p.threads = &threads;
    p.waitableOfHandle = [](std::uint32_t handle) -> std::optional<platform::Waitable> {
        if (handle == 0x44) {
            return platform::Waitable{9};
        }
        return std::nullopt;
    };
    std::vector<std::uint32_t> fallback;
    std::vector<std::uint32_t> waited;
    Environment env = environmentFor(
        p, [&](std::uint32_t ms) { waited.push_back(ms); },
        [&](std::uint32_t handle) { fallback.push_back(handle); });
    env.signal(0x44);
    env.signal(0x48);
    env.wait(10);
    CHECK(threads.log.Calls() == std::vector<std::string>{"SetEvent(9)"});
    CHECK(fallback == std::vector<std::uint32_t>{0x48});
    CHECK(waited == std::vector<std::uint32_t>{10});

    Platform bare;
    fallback.clear();
    Environment plain = environmentFor(bare, nullptr, [&](std::uint32_t h) { fallback.push_back(h); });
    plain.signal(0x44);
    CHECK(fallback == std::vector<std::uint32_t>{0x44});
}

TEST_CASE("with Threads, opening a session starts one service thread that pumps it") {
    platform::TransportMock transport;
    FakeClock clock;
    platform::ThreadsMock threads;
    Environment env;
    env.transport = &transport;
    env.clock = &clock;
    env.threads = &threads;
    std::uint32_t enumRequestsBefore = 0;
    {
        DirectPlay dp(env, Lan::testConfig());
        REQUIRE(host(dp, "TestGame") == kDpOk);
        CHECK(threads.log.Count("StartThread") == 1u);
        CHECK(threads.lastThreadOptions.name == "dplay");
        REQUIRE(threads.lastBody);
        // A datagram that comes in while the game makes no call is answered by the thread.
        transport.incoming.push_back(
            {concat({hex("53 46 44 50 01 01 00 00 05 00 00 00"), guidBytes(testApplication())}),
             platform::Endpoint{0xC0000214, 50001}});
        enumRequestsBefore = static_cast<std::uint32_t>(transport.sent.size());
        // The body loops until the object goes; run one pass of it by hand instead.
        dp.Service(0);
        CHECK(transport.sent.size() == enumRequestsBefore + 1u);
        REQUIRE(dp.Close() == kDpOk);
        REQUIRE(host(dp, "TestGame") == kDpErrAlreadyInitialized);
        SessionDesc d;
        d.application = testApplication();
        REQUIRE(dp.Open(d, kOpenCreate) == kDpOk);
        CHECK(threads.log.Count("StartThread") == 1u); // still the one thread
    }
    // Destroying the object waits for its thread and closes it.
    CHECK(threads.log.Count("Wait") == 1u);
    CHECK(threads.log.Count("Close") == 1u);
}

} // namespace sfcop::hle::dplay
