#include <doctest/doctest.h>

#include <cstring>

#include "Config.h"
#include "Health.h"

using namespace sfcop::auth;

namespace {

const char* fakeEnv(const char* name) {
    if (std::strcmp(name, "SFCOP_LOGIN_LISTEN") == 0) return "127.0.0.1:29900";
    if (std::strcmp(name, "SFCOP_PATCH_LISTEN") == 0) return "off";
    return nullptr;
}

}  // namespace

TEST_CASE("the defaults are the original's ports and the loopback health listener") {
    Options o;
    CHECK(o.login.port == 29900);
    CHECK(o.search.port == 29901);
    CHECK(o.patch.port == 80);
    CHECK_FALSE(o.patchOff);
    CHECK(o.health.address == "127.0.0.1");
    CHECK(o.health.port == 47714);
}

TEST_CASE("the environment sets the options and the command line overrides it") {
    Options o;
    std::string error;
    REQUIRE(applyEnvironment(o, fakeEnv, error));
    CHECK(o.login.address == "127.0.0.1");
    CHECK(o.patchOff);
    const char* argv[] = {"sfcop-auth", "--patch-listen", "127.0.0.1:47715", "--log-level", "warn"};
    REQUIRE(parseArgs(5, argv, o, error));
    CHECK_FALSE(o.patchOff);
    CHECK(o.patch.port == 47715);
    CHECK(o.logLevel == LogLevel::kWarn);
}

TEST_CASE("a bad option is named") {
    Options o;
    std::string error;
    const char* bad[] = {"x", "--login-listen", "localhost:29900"};
    CHECK_FALSE(parseArgs(3, bad, o, error));
    CHECK(error == "bad --login-listen");
    const char* unknown[] = {"x", "--key", "AAAA"};
    CHECK_FALSE(parseArgs(3, unknown, o, error));
    CHECK(error == "unknown option --key");
}

TEST_CASE("the health endpoint reports the accounts held") {
    HealthState s;
    s.accounts = 3;
    s.uptimeSeconds = 5;
    std::string ok = handleHealthRequest("GET /v1/health HTTP/1.1\r\n\r\n", s);
    CHECK(ok.find("{\"status\":\"ok\",\"api\":1,\"service\":\"auth\",\"accounts\":3,\"uptime_s\":5}") !=
          std::string::npos);
    CHECK(handleHealthRequest("GET / HTTP/1.1\r\n\r\n", s).rfind("HTTP/1.1 404", 0) == 0);
}
