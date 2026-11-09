#include <doctest/doctest.h>

#include <cstring>

#include "Config.h"
#include "Health.h"

using namespace sfcop::directory;

namespace {

const char* fakeEnv(const char* name) {
    if (std::strcmp(name, "SFCOP_DIRECTORY_LISTEN") == 0) return "127.0.0.1:15101";
    if (std::strcmp(name, "SFCOP_PUBLIC_ADDRESS") == 0) return "192.0.2.20";
    if (std::strcmp(name, "SFCOP_HEALTH_LISTEN") == 0) return "127.0.0.1:47799";
    return nullptr;
}

const char* badEnv(const char* name) {
    return std::strcmp(name, "SFCOP_DETECTOR_LISTEN") == 0 ? "0.0.0.0:99999" : nullptr;
}

}  // namespace

TEST_CASE("the defaults are the original's port and the loopback health listener") {
    Options o;
    CHECK(o.listen.address == "0.0.0.0");
    CHECK(o.listen.port == 15101);
    CHECK(o.health.address == "127.0.0.1");
    CHECK(o.health.port == 47713);
    CHECK(o.campaignPath == "/StarFleetCommand2/Game/Release");
}

TEST_CASE("the environment sets the options and the command line overrides it") {
    Options o;
    std::string error;
    REQUIRE(applyEnvironment(o, fakeEnv, error));
    CHECK(o.listen.address == "127.0.0.1");
    CHECK(o.publicAddress == "192.0.2.20");
    CHECK(o.health.port == 47799);
    const char* argv[] = {"sfcop-directory", "--public-address", "198.51.100.4", "--campaigns",
                          "/tmp/x.txt", "--log-level", "debug"};
    REQUIRE(parseArgs(7, argv, o, error));
    CHECK(o.publicAddress == "198.51.100.4");
    CHECK(o.campaignFile == "/tmp/x.txt");
    CHECK(o.logLevel == LogLevel::kDebug);
}

TEST_CASE("a bad variable or option is named") {
    Options o;
    std::string error;
    CHECK_FALSE(applyEnvironment(o, badEnv, error));
    CHECK(error == "bad SFCOP_DETECTOR_LISTEN");
    const char* unknown[] = {"x", "--colour", "red"};
    CHECK_FALSE(parseArgs(3, unknown, o, error));
    CHECK(error == "unknown option --colour");
    const char* missing[] = {"x", "--listen"};
    CHECK_FALSE(parseArgs(2, missing, o, error));
    const char* zero[] = {"x", "--public-address", "0.0.0.0"};
    CHECK_FALSE(parseArgs(3, zero, o, error));
    const char* path[] = {"x", "--campaign-path", "relative"};
    CHECK_FALSE(parseArgs(3, path, o, error));
}

TEST_CASE("the health endpoint answers GET /v1/health only") {
    HealthState s;
    s.entries = 2;
    s.uptimeSeconds = 30;
    std::string ok = handleHealthRequest("GET /v1/health HTTP/1.1\r\nHost: x\r\n\r\n", s);
    CHECK(ok.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    CHECK(ok.find("\r\n\r\n{\"status\":\"ok\",\"api\":1,\"service\":\"directory\",\"entries\":2,"
                  "\"uptime_s\":30}") != std::string::npos);
    CHECK(handleHealthRequest("GET /v1/other HTTP/1.1\r\n\r\n", s).rfind("HTTP/1.1 404", 0) == 0);
    CHECK(handleHealthRequest("POST /v1/health HTTP/1.1\r\n\r\n", s).rfind("HTTP/1.1 405", 0) == 0);
    CHECK(handleHealthRequest("nonsense", s).rfind("HTTP/1.1 400", 0) == 0);
}
