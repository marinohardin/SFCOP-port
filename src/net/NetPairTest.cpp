// Two native clients in two processes reach each other through the Rendezvous, each behind a
// simulated NAT on loopback (tools/nettest/NetPair.h): directly when the NATs allow the punch,
// through the stand-in relay within 5 s when they do not, and through the relay at once when the
// test switch forces it. Real sockets, real processes and the steady clock: not a unit test, so
// these cases run only when SFCOP_NETTEST=1 is set (tools/nettest/README.md); sh scripts/check.sh
// and ctest skip them. The unit coverage of the same paths, on simulated time, is
// RendezvousEngineTest. On a platform without the host backend or fork(), the test case prints
// "skipped: ..." and returns.
#include <doctest/doctest.h>

#if __has_include("platform/Transport/TransportHost.cpp") && !defined(_WIN32)
#define SFCOP_NET_PAIR 1
#else
#define SFCOP_NET_PAIR 0
#endif

#if SFCOP_NET_PAIR

#include "../../tools/nettest/NetPair.h"

#include <cstdlib>
#include <cstring>

namespace sfcop::nettest {
namespace {

// True unless SFCOP_NETTEST=1 asks for the two-process runs.
bool netPairSkipped() {
    const char *value = std::getenv("SFCOP_NETTEST");
    return !value || std::strcmp(value, "1") != 0;
}

PairResult run(Scenario scenario) {
    PairResult result = runPair(scenario);
    const std::string name = scenarioName(scenario);
    const std::string line =
        result.ok ? name + ": " + result.path + " in " + std::to_string(result.connectMs) + " ms"
                  : name + ": " + result.failure;
    MESSAGE(line);
    return result;
}

bool skipped(const PairResult &result) {
    return result.failure.rfind("skipped", 0) == 0;
}

} // namespace

TEST_CASE("net pair: behind two cone NATs the clients connect directly" *
          doctest::skip(netPairSkipped())) {
    PairResult result = run(Scenario::kCone);
    if (skipped(result)) {
        return;
    }
    REQUIRE(result.ok);
    CHECK(result.path == "direct");
    CHECK(result.connectMs < 3000);
}

TEST_CASE("net pair: behind a symmetric NAT the clients connect through the relay within 5 s" *
          doctest::skip(netPairSkipped())) {
    PairResult result = run(Scenario::kSymmetric);
    if (skipped(result)) {
        return;
    }
    REQUIRE(result.ok);
    CHECK(result.path == "relay");
    CHECK(result.connectMs >= 3000);
    CHECK(result.connectMs < 5000);
}

TEST_CASE("net pair: with the punch blocked the clients connect through the relay within 5 s" *
          doctest::skip(netPairSkipped())) {
    PairResult result = run(Scenario::kBlocked);
    if (skipped(result)) {
        return;
    }
    REQUIRE(result.ok);
    CHECK(result.path == "relay");
    CHECK(result.connectMs >= 3000);
    CHECK(result.connectMs < 5000);
}

TEST_CASE("net pair: the test switch takes the relay at once" * doctest::skip(netPairSkipped())) {
    PairResult result = run(Scenario::kForced);
    if (skipped(result)) {
        return;
    }
    REQUIRE(result.ok);
    CHECK(result.path == "relay");
    CHECK(result.connectMs < 1500);
}

} // namespace sfcop::nettest

#else

TEST_CASE("net pair: two clients through the Rendezvous") {
    MESSAGE("skipped: needs the host Transport backend and fork()");
}

#endif
