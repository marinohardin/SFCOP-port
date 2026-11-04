// Two native clients on one machine, each its own process on the real Transport backend: the
// parent hosts a session, a forked child enumerates it, joins it, and the two exchange the game's
// first three message kinds (analysis/netcode/directplay.md: the data bundle, type 1, sent
// guaranteed with SendEx; the probe, type 2, with Send; the probe's reply, type 3), each side
// checking the other's bytes. It runs on loopback, on an enumeration port of its own.
//
// This is the one two-process test, and so the one test that may use a real clock
// (docs/CONVENTIONS.md §6): the two processes talk over real sockets and wait on the steady clock,
// each step bounded by a deadline. It needs the host Transport backend
// (src/platform/Transport/TransportHost.cpp) and fork(); on a platform without either, or where
// the backend opens no UDP socket, the test case prints "skipped: ..." and returns, so that a
// skip shows in the output and is never a silent pass.
#include <doctest/doctest.h>

#if __has_include("platform/Transport/TransportHost.cpp") && !defined(_WIN32)
#define SFCOP_DPLAY_LAN_PAIR 1
#else
#define SFCOP_DPLAY_LAN_PAIR 0
#endif

#if SFCOP_DPLAY_LAN_PAIR

#include "hle/dplay/Address.h"
#include "hle/dplay/DirectPlay.h"
#include "platform/Clock/Clock.h"
#include "platform/Transport/Transport.h"

#include <chrono>
#include <optional>
#include <thread>
#include <vector>

#include <csignal>

#include <sys/wait.h>
#include <unistd.h>

namespace sfcop::hle::dplay {

namespace {

// A clock on the host's steady clock: this test runs in real time.
class SteadyClock final : public platform::Clock {
public:
    platform::Result<std::uint32_t> Ticks() override {
        return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
    }
    platform::Result<std::uint64_t> Counter() override { return std::uint64_t{Ticks().Value()}; }
    platform::Result<std::uint64_t> CounterFrequency() override { return std::uint64_t{1000}; }
    platform::Result<platform::CivilTime> Now(platform::TimeZone) override {
        return platform::Error::kUnsupported;
    }
    platform::Result<std::int64_t> UnixTime() override { return platform::Error::kUnsupported; }
    platform::Status Sleep(std::uint32_t ms) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return {};
    }
};

// {5346434F-5000-4C41-4E50-414952544553}: this test's application.
constexpr Guid kApplication =
    makeGuid(0x5346434F, 0x5000, 0x4C41, {0x4E, 0x50, 0x41, 0x49, 0x52, 0x54, 0x45, 0x53});

// Type 1: one record of 4 bytes.
const std::vector<std::uint8_t> kBundle = {0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00,
                                           0x00, 0x04, 0x00, 0xAA, 0xBB, 0xCC, 0xDD};
// Type 2: sent at 1000 ms, 0, probe 1.
const std::vector<std::uint8_t> kProbe = {0x02, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00,
                                          0x00, 0xE8, 0x03, 0x00, 0x00, 0x00, 0x00,
                                          0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
// Type 3: the time back, an id, probe 1.
const std::vector<std::uint8_t> kProbeReply = {0x03, 0x00, 0x00, 0x00, 0x0C, 0x00, 0x00,
                                               0x00, 0xE8, 0x03, 0x00, 0x00, 0x01, 0x00,
                                               0x02, 0x00, 0x01, 0x00, 0x00, 0x00};

constexpr std::uint32_t kDeadlineMs = 10000;

// Kills and reaps the child when the parent leaves the test early (a failed REQUIRE), so that it
// is not left to run to its own deadline.
class ChildGuard {
public:
    explicit ChildGuard(pid_t pid) : pid_(pid) {}
    ~ChildGuard() {
        if (pid_ > 0) {
            kill(pid_, SIGKILL);
            int ignored = 0;
            (void)waitpid(pid_, &ignored, 0);
        }
    }
    ChildGuard(const ChildGuard &) = delete;
    ChildGuard &operator=(const ChildGuard &) = delete;
    // The child ended and was reaped by the caller.
    void Reaped() { pid_ = -1; }

private:
    pid_t pid_;
};

Config pairConfig(std::uint16_t port) {
    Config c;
    c.enumPort = port;
    c.searchLoopback = true;
    c.broadcastAddress = 0x7F000001; // stay on this machine
    return c;
}

std::optional<ReceivedMessage> receive(DirectPlay &dp) {
    Dpid from = 0, to = 0;
    std::uint32_t size = 0xFFFFFFFF;
    ReceivedMessage m;
    if (dp.Receive(&from, &to, kReceiveAll, &size, &m) != kDpOk) {
        return std::nullopt;
    }
    return m;
}

// The joiner, in the child process. Returns its exit code: 0 when every step worked.
int runJoiner(std::uint16_t port) {
    std::unique_ptr<platform::Transport> transport = platform::makeTransport();
    SteadyClock clock;
    Environment env;
    env.transport = transport.get();
    env.clock = &clock;
    DirectPlay dp(env, pairConfig(port));
    std::vector<std::uint8_t> connection = tcpIpConnection();
    if (dp.InitializeConnection(connection.data(), connection.size(), 0) != kDpOk) {
        return 10;
    }
    std::optional<SessionDesc> found;
    std::uint32_t start = clock.Ticks().Value();
    while (!found && clock.Ticks().Value() - start < kDeadlineMs) {
        SessionDesc filter;
        filter.application = kApplication;
        if (dp.EnumSessions(filter, 500,
                            [&](const SessionDesc *s, std::uint32_t &, std::uint32_t) {
                                if (s != nullptr && !found) {
                                    found = *s;
                                }
                                return false;
                            },
                            0) != kDpOk) {
            return 11;
        }
    }
    if (!found || found->name != std::optional<std::string>("LanPair")) {
        return 12;
    }
    SessionDesc join;
    join.instance = found->instance;
    if (dp.Open(join, kOpenJoin) != kDpOk) {
        return 13;
    }
    Dpid helm = 0;
    Name name;
    name.shortName = "Helm";
    if (dp.CreatePlayer(&helm, name, 0, {}, 0) != kDpOk) {
        return 14;
    }
    Dpid captain = 0;
    dp.EnumPlayers(nullptr, [&](const PlayerEntry &p) {
        captain = p.id;
        return false;
    }, kEnumPlayersRemote);
    if (captain == 0) {
        return 15;
    }
    std::uint32_t id = 0;
    if (dp.SendEx(helm, captain, 0x601, kBundle.data(), kBundle.size(), 0, 1000, &id) !=
        kDpErrPending) {
        return 16;
    }
    if (dp.Send(helm, captain, 0, kProbe.data(), kProbe.size()) != kDpOk) {
        return 17;
    }
    start = clock.Ticks().Value();
    while (clock.Ticks().Value() - start < kDeadlineMs) {
        dp.Service(10);
        while (std::optional<ReceivedMessage> m = receive(dp)) {
            if (m->from == captain) {
                if (m->blob.bytes != kProbeReply) {
                    return 18;
                }
                // Stay a little, so that the host has its acknowledgements, then leave.
                for (int i = 0; i < 30; ++i) {
                    dp.Service(10);
                }
                dp.Close();
                return 0;
            }
        }
    }
    return 19;
}

} // namespace

TEST_CASE("two clients on one machine: enumerate, join, exchange the first three message kinds") {
    std::unique_ptr<platform::Transport> transport = platform::makeTransport();
    {
        platform::Result<platform::SocketId> probe = transport->Open(platform::SocketKind::kUdp);
        if (!probe.Ok()) {
            MESSAGE("skipped: the Transport backend opens no UDP socket on this platform");
            return;
        }
        (void)transport->Close(probe.Value());
    }
    const std::uint16_t port = static_cast<std::uint16_t>(47710 + getpid() % 200);
    SteadyClock clock;
    Environment env;
    env.transport = transport.get();
    env.clock = &clock;
    DirectPlay dp(env, pairConfig(port));
    std::vector<std::uint8_t> connection = tcpIpConnection();
    REQUIRE(dp.InitializeConnection(connection.data(), connection.size(), 0) == kDpOk);
    SessionDesc desc;
    desc.flags = kSessionDirectPlayProtocol | kSessionKeepAlive | kSessionMigrateHost;
    desc.application = kApplication;
    desc.maxPlayers = 8;
    desc.name = "LanPair";
    REQUIRE(dp.Open(desc, kOpenCreate) == kDpOk);
    Dpid captain = 0;
    Name name;
    name.shortName = "TestCaptain";
    REQUIRE(dp.CreatePlayer(&captain, name, 0, {}, 0) == kDpOk);

    pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        _exit(runJoiner(port));
    }
    ChildGuard guard(child);

    Dpid helm = 0;
    bool gotBundle = false, gotProbe = false, joinerLeft = false;
    std::uint32_t start = clock.Ticks().Value();
    while (!joinerLeft && clock.Ticks().Value() - start < kDeadlineMs + 2000) {
        dp.Service(10);
        while (std::optional<ReceivedMessage> m = receive(dp)) {
            const std::vector<std::uint8_t> &b = m->blob.bytes;
            std::uint32_t type = b.size() >= 4 ? b[0] | b[1] << 8 : 0;
            if (m->from == kDpidSysMsg && type == kSysCreatePlayerOrGroup && b.size() >= 12) {
                helm = b[8] | b[9] << 8 | b[10] << 16 | static_cast<std::uint32_t>(b[11]) << 24;
            } else if (m->from == kDpidSysMsg && type == kSysDestroyPlayerOrGroup) {
                joinerLeft = true;
            } else if (m->from == helm && helm != 0 && b == kBundle) {
                gotBundle = true;
            } else if (m->from == helm && helm != 0 && b == kProbe) {
                gotProbe = true;
                REQUIRE(dp.Send(captain, helm, 0, kProbeReply.data(), kProbeReply.size()) ==
                        kDpOk);
            } else {
                FAIL_CHECK("an unexpected message from " << m->from);
            }
        }
    }
    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    guard.Reaped();
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0); // see runJoiner() for what each code means
    CHECK(helm == 0x00020001u);
    CHECK(gotBundle);
    CHECK(gotProbe);
    CHECK(joinerLeft);
}

} // namespace sfcop::hle::dplay

#else

TEST_CASE("two clients on one machine: enumerate, join, exchange the first three message kinds") {
    MESSAGE("skipped: needs the host Transport backend (src/platform/Transport/TransportHost.cpp) "
            "and fork()");
}

#endif
