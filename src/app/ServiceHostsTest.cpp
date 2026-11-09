#include "app/ServiceHosts.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

using namespace sfcop::app;

TEST_CASE("the services object of servers.json names each stand-in's host") {
    ServiceHosts hosts;
    std::string error;
    REQUIRE(parseServiceHosts(R"({
  "version": 1,
  "lobbies": [ { "name": "Official", "url": "https://lobby.example.test" } ],
  "rendezvous": null,
  "services": { "directory": "192.0.2.10", "auth": "auth.example.test",
                "cdkey": "192.0.2.11", "patch": "192.0.2.12", "future": "ignored" }
})",
                              hosts, error));
    CHECK(hosts.directory == "192.0.2.10");
    CHECK(hosts.auth == "auth.example.test");
    CHECK(hosts.cdkey == "192.0.2.11");
    CHECK(hosts.patch == "192.0.2.12");
}

TEST_CASE("a service the file leaves out, or names with a non-string, keeps the default host") {
    ServiceHosts hosts;
    std::string error;
    REQUIRE(parseServiceHosts(R"({"services": {"directory": "127.0.0.1", "auth": 7, "patch": ""},
                                 "rendezvous": {"host": "x", "port": 47710}})",
                              hosts, error));
    CHECK(hosts.directory == "127.0.0.1");
    CHECK(hosts.auth == "services.example.test");
    CHECK(hosts.cdkey == "services.example.test");
    CHECK(hosts.patch == "services.example.test");
    REQUIRE(parseServiceHosts(R"({"version": 1})", hosts, error));
}

TEST_CASE("malformed text is refused and leaves the hosts as they were") {
    ServiceHosts hosts;
    hosts.directory = "kept";
    std::string error;
    CHECK_FALSE(parseServiceHosts(R"({"services": {"directory": "127.0.0.1")", hosts, error));
    CHECK(error == "not a JSON object");
    CHECK_FALSE(parseServiceHosts("[1, 2]", hosts, error));
    CHECK_FALSE(parseServiceHosts(R"({"services": {}} trailing)", hosts, error));
    CHECK(hosts.directory == "kept");
}

TEST_CASE("every host name the original carries is aliased to its stand-in") {
    ServiceHosts hosts;
    hosts.directory = "192.0.2.1";
    hosts.auth = "192.0.2.2";
    hosts.cdkey = "192.0.2.3";
    hosts.patch = "192.0.2.4";
    std::map<std::string, std::string> expected = {
        {"sfc2.west.won.net", "192.0.2.1"},   {"sfc2.central.won.net", "192.0.2.1"},
        {"sfc2.east.won.net", "192.0.2.1"},   {"master.gamespy.com", "192.0.2.1"},
        {"gpcm.gamespy.com", "192.0.2.2"},    {"gpsp.gamespy.com", "192.0.2.2"},
        {"key.gamespy.com", "192.0.2.3"},     {"motd.gamespy.com", "192.0.2.4"},
    };
    CHECK(serviceAliases(hosts) == expected);
}

TEST_CASE("the data folder's servers.json is read; a missing one gives the defaults") {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "sfcop-servicehosts-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::string warning;
    ServiceHosts none = loadServiceHosts(dir.string(), warning);
    CHECK(none.directory == "services.example.test");
    CHECK(warning.empty());
    std::ofstream(dir / "servers.json") << R"({"services": {"directory": "127.0.0.1"}})";
    ServiceHosts some = loadServiceHosts(dir.string(), warning);
    CHECK(some.directory == "127.0.0.1");
    CHECK(warning.empty());
    std::ofstream(dir / "servers.json") << "{oops";
    ServiceHosts bad = loadServiceHosts(dir.string(), warning);
    CHECK(bad.directory == "services.example.test");
    CHECK(warning ==
          "servers.json is not valid JSON; no service is reachable until servers.json is fixed");
    fs::remove_all(dir);
}
