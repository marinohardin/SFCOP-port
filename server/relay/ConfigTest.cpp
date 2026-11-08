#include <doctest/doctest.h>

#include "Config.h"

using namespace sfcop::relay;

TEST_CASE("the ticket key is exactly 64 hexadecimal digits of either case") {
    Bytes k;
    REQUIRE(parseHexKey("000102030405060708090A0b0c0d0e0f101112131415161718191a1b1c1d1e1f", k));
    REQUIRE(k.size() == 32);
    CHECK(k[0] == 0x00);
    CHECK(k[10] == 0x0a);
    CHECK(k[11] == 0x0b);
    CHECK(k[31] == 0x1f);
    CHECK_FALSE(parseHexKey("0001", k));
    CHECK_FALSE(parseHexKey(std::string(63, 'a') + "g", k));
    CHECK_FALSE(parseHexKey(std::string(65, 'a'), k));
}

TEST_CASE("dotted quads parse to host-order values and bad ones are refused") {
    std::uint32_t a = 0;
    REQUIRE(parseIPv4("192.0.2.1", a));
    CHECK(a == 0xC0000201);
    CHECK_FALSE(parseIPv4("192.0.2", a));
    CHECK_FALSE(parseIPv4("192.0.2.256", a));
    CHECK_FALSE(parseIPv4("192.0.2.1.5", a));
    CHECK_FALSE(parseIPv4("192.0..1", a));
    CHECK_FALSE(parseIPv4("", a));
}

TEST_CASE("a block list skips comments and blank lines and one bad line fails the whole file") {
    std::unordered_set<std::uint32_t> set;
    std::string error;
    REQUIRE(parseBlocklist("# abusers\n198.51.100.7  # one\n\n203.0.113.9\r\n", set, error));
    CHECK(set.size() == 2);
    CHECK(set.count(0xC6336407) == 1);
    CHECK(set.count(0xCB007109) == 1);

    std::unordered_set<std::uint32_t> kept = {1};
    CHECK_FALSE(parseBlocklist("198.51.100.7\nnot an address\n", kept, error));
    CHECK(error == "line 2 is not an IPv4 address");
    CHECK(kept.size() == 1);  // untouched on failure
}

TEST_CASE("the command line sets ports, addresses and the log level, and refuses the rest") {
    const char* argv[] = {"sfcop-relay", "--port", "5000", "--health-port", "5001", "--bind",
                          "127.0.0.1", "--log-level", "debug", "--blocklist", "block.txt"};
    Options o;
    std::string error;
    REQUIRE(parseArgs(11, argv, o, error));
    CHECK(o.port == 5000);
    CHECK(o.healthPort == 5001);
    CHECK(o.bindAddress == "127.0.0.1");
    CHECK(o.healthAddress == "127.0.0.1");
    CHECK(o.logLevel == LogLevel::kDebug);
    CHECK(o.blocklistPath == "block.txt");

    const char* bad1[] = {"sfcop-relay", "--port", "70000"};
    const char* bad2[] = {"sfcop-relay", "--nope", "1"};
    const char* bad3[] = {"sfcop-relay", "--port"};
    Options p;
    CHECK_FALSE(parseArgs(3, bad1, p, error));
    CHECK_FALSE(parseArgs(3, bad2, p, error));
    CHECK_FALSE(parseArgs(2, bad3, p, error));

    Options defaults;
    CHECK(defaults.port == 47710);
    CHECK(defaults.healthPort == 47711);
    CHECK(defaults.healthAddress == "127.0.0.1");
}

namespace {
const char* fakeEnv(const char* name) {
    std::string n = name;
    if (n == "SFCOP_RELAY_PORT") return "48000";
    if (n == "SFCOP_HEALTH_LISTEN") return "127.0.0.2:48001";
    if (n == "SFCOP_LOG_LEVEL") return "warn";
    return nullptr;
}
const char* noEnv(const char*) { return nullptr; }
const char* badEnv(const char*) { return "banana"; }
}  // namespace

TEST_CASE("the ports come from the environment file's names, and the command line wins") {
    Options o;
    std::string error;
    REQUIRE(applyEnvironment(o, fakeEnv, error));
    CHECK(o.port == 48000);
    CHECK(o.healthPort == 48001);
    CHECK(o.healthAddress == "127.0.0.2");
    CHECK(o.logLevel == LogLevel::kWarn);
    const char* argv[] = {"sfcop-relay", "--port", "49000"};
    REQUIRE(parseArgs(3, argv, o, error));
    CHECK(o.port == 49000);
    CHECK(o.healthPort == 48001);

    Options d;
    REQUIRE(applyEnvironment(d, noEnv, error));
    CHECK(d.port == 47710);
    CHECK_FALSE(applyEnvironment(d, badEnv, error));
    CHECK(error == "bad SFCOP_RELAY_PORT");
}
