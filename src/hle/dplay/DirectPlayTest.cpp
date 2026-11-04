// The DirectPlay object's lifetime against its service thread, on mocks.
#include "hle/dplay/DirectPlay.h"

#include "hle/dplay/DplayMockFixture.h"
#include "platform/Threads/Threads.h"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <algorithm>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace sfcop::hle::dplay {

using namespace test;

namespace {

// A Threads that keeps the body StartThread() is given (the test starts it when it chooses, on a
// real thread) and whose Wait() joins that thread. It records the timeout it was given, and a
// wait with a time limit gives up after at most 300 ms of real time, however long the limit.
// The rest is not supported.
class JoiningThreads final : public platform::Threads {
public:
    std::function<std::uint32_t()> lastBody;
    std::thread worker;
    std::atomic<bool> bodyFinished{false};
    std::optional<std::uint32_t> waitTimeout = std::uint32_t{1};

    void StartBody() {
        auto body = lastBody;
        worker = std::thread([this, body]() {
            body();
            bodyFinished = true;
        });
    }

    platform::Result<platform::Waitable> StartThread(std::function<std::uint32_t()> body,
                                                     const platform::ThreadOptions &) override {
        lastBody = std::move(body);
        return platform::Waitable{1};
    }
    platform::Result<platform::WaitResult> Wait(const platform::Waitable *, std::size_t,
                                                platform::WaitMode,
                                                std::optional<std::uint32_t> timeoutMs) override {
        waitTimeout = timeoutMs;
        auto limit = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(timeoutMs ? std::min(*timeoutMs, 300u) : 20000);
        while (!bodyFinished && std::chrono::steady_clock::now() < limit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!bodyFinished) {
            return platform::WaitResult{platform::WaitOutcome::kTimedOut, 0};
        }
        if (worker.joinable()) {
            worker.join();
        }
        return platform::WaitResult{platform::WaitOutcome::kSignaled, 0};
    }
    platform::Status Close(platform::Waitable) override { return {}; }

    platform::Result<std::uint32_t> GetExitCode(platform::Waitable) override { return U(); }
    platform::Status ResumeThread(platform::Waitable) override { return Unsupported(); }
    platform::Status SetThreadPriority(platform::Waitable, platform::ThreadPriority) override {
        return Unsupported();
    }
    platform::Result<std::uint32_t> CurrentThreadId() override { return U(); }
    platform::Result<platform::Created> MakeEvent(bool, bool, std::string_view) override {
        return platform::Error::kUnsupported;
    }
    platform::Status SetEvent(platform::Waitable) override { return Unsupported(); }
    platform::Status ResetEvent(platform::Waitable) override { return Unsupported(); }
    platform::Result<platform::Created> MakeMutex(bool, std::string_view) override {
        return platform::Error::kUnsupported;
    }
    platform::Status ReleaseMutex(platform::Waitable) override { return Unsupported(); }
    platform::Result<platform::Created> MakeSemaphore(std::int32_t, std::int32_t,
                                                      std::string_view) override {
        return platform::Error::kUnsupported;
    }
    platform::Result<std::int32_t> ReleaseSemaphore(platform::Waitable, std::int32_t) override {
        return platform::Error::kUnsupported;
    }
    platform::Result<platform::Waitable> Open(std::string_view) override {
        return platform::Error::kUnsupported;
    }
    platform::Result<std::uint32_t> AllocateTlsSlot() override { return U(); }
    platform::Status FreeTlsSlot(std::uint32_t) override { return Unsupported(); }
    platform::Status SetTlsValue(std::uint32_t, std::uint64_t) override { return Unsupported(); }
    platform::Result<std::uint64_t> GetTlsValue(std::uint32_t) override {
        return platform::Error::kUnsupported;
    }

private:
    static platform::Status Unsupported() { return platform::Error::kUnsupported; }
    static platform::Error U() { return platform::Error::kUnsupported; }
};

} // namespace

TEST_CASE("Release during a pending delivery joins the service thread before freeing") {
    JoiningThreads threads;
    std::mutex machine; // the game's machine: the thread running Release holds it
    std::atomic<bool> signalEntered{false};
    std::atomic<bool> signalled{false};
    std::unique_ptr<MockJoined> j = std::make_unique<MockJoined>(
        Lan::testConfig(), [&](Environment &env) {
            env.threads = &threads;
            env.signal = [&](std::uint32_t) {
                signalEntered = true;
                std::lock_guard<std::mutex> enter(machine); // as the guest bridge's mach_enter()
                signalled = true;
            };
            env.blocking = [&](const std::function<void()> &work) {
                machine.unlock(); // hle_block_begin()
                work();
                machine.lock(); // hle_block_end()
            };
        });
    REQUIRE(threads.lastBody);
    Dpid helm = 0;
    REQUIRE(j->dp->CreatePlayer(&helm, Name{}, 0x77, {}, 0) == kDpOk);
    REQUIRE(helm == 0x00020001u);

    machine.lock(); // the guest thread is running Release()
    // A message for the player is waiting when the service thread starts.
    j->transport.incoming.push_back(
        {mockReliable(2, concat({hex("0B 01 00 01 00 01 00 02 00"), hex("01 00 00 00 00 00")})),
         kHostEndpoint});
    threads.StartBody();
    for (int i = 0; i < 5000 && !signalEntered; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(signalEntered); // the thread is waiting for the machine, inside Environment::signal

    j->dp.reset(); // Release() returned 0
    bool finishedBeforeFree = threads.bodyFinished;
    std::optional<std::uint32_t> timeout = threads.waitTimeout;
    machine.unlock();
    if (threads.worker.joinable()) {
        threads.worker.join();
    }
    CHECK(finishedBeforeFree);
    CHECK(timeout == std::nullopt); // no time limit
    CHECK(signalled);
}

namespace {

std::vector<std::uint8_t> le32(std::uint32_t v) {
    return {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8),
            static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 24)};
}

// Runs the object until the mock has no datagram left.
void serviceUntilEmpty(MockJoined &j) {
    for (int i = 0; i < 100 && !j.transport.incoming.empty(); ++i) {
        j.dp->Service(0);
    }
    REQUIRE(j.transport.incoming.empty());
}

} // namespace

TEST_CASE("the message inbox keeps the newest kMaxInbox messages and counts the rest") {
    MockJoined j;
    Dpid helm = 0;
    REQUIRE(j.dp->CreatePlayer(&helm, Name{}, 0, {}, 0) == kDpOk);
    const std::size_t sent = kMaxInbox + 100;
    for (std::size_t i = 0; i < sent; ++i) {
        // An unreliable game message from the host's player to the helm, 4 bytes: the index.
        j.transport.incoming.push_back(
            {concat({hex("53 46 44 50 01 07 00 00 0D F0 AD 0B 01 00 00 00"), hex("01 00 01 00"),
                     le32(helm), le32(static_cast<std::uint32_t>(i))}),
             kHostEndpoint});
    }
    serviceUntilEmpty(j);
    CHECK(j.dp->GetCounters().droppedMessages == 100u);
    std::vector<ReceivedMessage> got = drain(*j.dp);
    REQUIRE(got.size() == kMaxInbox);
    CHECK(got.front().blob.bytes == le32(100)); // the oldest hundred went
    CHECK(got.back().blob.bytes == le32(static_cast<std::uint32_t>(sent - 1)));
}

TEST_CASE("enumeration remembers kMaxFound sessions, the newest, and counts the rest") {
    platform::TransportMock transport;
    FakeClock clock;
    Environment env;
    env.transport = &transport;
    env.clock = &clock;
    DirectPlay dp(env, Lan::testConfig());
    REQUIRE(initialize(dp) == kDpOk);
    const std::uint32_t replies = static_cast<std::uint32_t>(kMaxFound) + 44;
    for (std::uint32_t i = 0; i < replies; ++i) {
        // Enumeration reply to request 1, from a session whose instance GUID is i + 1.
        transport.incoming.push_back(
            {concat({hex("53 46 44 50 01 02 00 00 01 00 00 00 44 20 00 00"), le32(i + 1),
                     hex("00 00 00 00 00 00 00 00 00 00 00 00"), guidBytes(testApplication()),
                     hex("08 00 00 00 01 00 00 00"),
                     hex("01 00 00 00 02 00 00 00 03 00 00 00 04 00 00 00"),
                     hex("08 00 54 65 73 74 47 61 6D 65 FF FF")}),
             platform::Endpoint{0xC0000200 + (i & 0xFF), 2300}});
    }
    std::size_t listed = 0;
    SessionDesc filter;
    REQUIRE(dp.EnumSessions(
                filter, 100,
                [&](const SessionDesc *s, std::uint32_t &, std::uint32_t) {
                    if (s != nullptr) {
                        ++listed;
                    }
                    return s != nullptr; // on to the next; the timeout call ends it
                },
                0) == kDpOk);
    CHECK(transport.incoming.empty());
    CHECK(listed == kMaxFound);
    CHECK(dp.GetCounters().droppedSessions == 44u);
}

TEST_CASE("unacknowledged channel messages are capped per peer, the oldest dropped") {
    MockJoined j;
    Dpid helm = 0;
    REQUIRE(j.dp->CreatePlayer(&helm, Name{}, 0, {}, 0) == kDpOk);
    std::vector<std::uint8_t> payload = {1, 0, 0, 0};
    for (std::size_t i = 0; i < kMaxUnacked + 5; ++i) {
        REQUIRE(j.dp->Send(helm, 0x00010001, kSendGuaranteed, payload.data(), payload.size()) ==
                kDpOk);
    }
    // The player's announcement is the first message in the queue: six past the cap in all.
    CHECK(j.dp->GetCounters().droppedUnacked == 6u);
    std::uint32_t waiting = 0;
    REQUIRE(j.dp->GetMessageQueue(0, 0, kMessageQueueSend, &waiting, nullptr) == kDpOk);
    CHECK(waiting == kMaxUnacked);
}

TEST_CASE("a peer may announce kMaxEntitiesPerPeer players and groups, no more") {
    MockJoined j;
    j.transport.incoming.push_back(
        {mockReliable(2, hex("02 03 00 00 00 0B 02 00 C0 FD 08")), kHostEndpoint});
    serviceUntilEmpty(j);
    const platform::Endpoint peer3{0xC000020B, 2301};
    const std::uint32_t announced = static_cast<std::uint32_t>(kMaxEntitiesPerPeer) + 3;
    for (std::uint32_t i = 0; i < announced; ++i) {
        // Peer 3 adds a player with id 0x0003xxxx owned by peer 3, with no name or data.
        std::vector<std::uint8_t> inner =
            concat({hex("04"), le32(0x00030001 + i), hex("00 00 00 00 00 03 00 00 00"),
                    hex("FF FF FF FF 00 00 00 00 00 00")});
        j.transport.incoming.push_back(
            {concat({hex("53 46 44 50 01 05 00 00 0D F0 AD 0B 03 00 00 00"), le32(i + 1), inner}),
             peer3});
    }
    serviceUntilEmpty(j);
    CHECK(j.dp->GetCounters().rejectedEntities == 3u);
    std::size_t listed = 0;
    REQUIRE(j.dp->EnumPlayers(nullptr, [&](const PlayerEntry &) {
        ++listed;
        return true;
    }, 0) == kDpOk);
    CHECK(listed == kMaxEntitiesPerPeer + 1); // and the host's captain
}

} // namespace sfcop::hle::dplay
