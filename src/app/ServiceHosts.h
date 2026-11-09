// Where the client finds the online services that stand in for the original ones (DESIGN §5.13,
// src/net/DIRECTORY_API.md, "Pointing the client at the stand-ins").
//
// The original reaches its online services by host names it carries: the directory's from its
// server setup file, the others written into the exe. Those services no longer exist, so the
// client resolves each of those names as the host of this project's stand-in, which the player's
// `servers.json` names (server/README.md, "The client's configuration"):
//
//   { "version": 1, "lobbies": [ ... ],
//     "services": { "directory": "127.0.0.1", "auth": "127.0.0.1",
//                   "cdkey": "127.0.0.1", "patch": "127.0.0.1" } }
//
// A service the file does not name, or no file at all, uses kDefaultServiceHost, which resolves
// to nothing: the client fails closed (the integrator's decision; the original hosts are gone). The winsock
// module applies the map (hle/winsock: setGuestHostAliases).
#pragma once

#include <map>
#include <string>

namespace sfcop::app {

// The compiled-in host of every stand-in until the owner's hosted address is decided; a name in
// a reserved test domain, so a client without a configuration can never reach anyone else's
// server and shows the original's could-not-connect state instead.
inline constexpr const char *kDefaultServiceHost = "services.example.test";

struct ServiceHosts {
    std::string directory = kDefaultServiceHost; // the directory and its firewall detector
    std::string auth = kDefaultServiceHost;      // the account login service
    std::string cdkey = kDefaultServiceHost;     // the CD-key service
    std::string patch = kDefaultServiceHost;     // the version check
};

// Reads the "services" object of a servers.json text. Unknown keys are ignored; a value that is
// not a non-empty string keeps the default. False, with the defaults kept and `error` set, when
// the text is not a JSON object.
bool parseServiceHosts(const std::string &json, ServiceHosts &out, std::string &error);

// The alias map for the winsock module: each original host name -> the stand-in's host.
std::map<std::string, std::string> serviceAliases(const ServiceHosts &hosts);

// Reads <dataDir>/servers.json; a missing or malformed file gives the defaults, and a malformed
// one is reported in `warning` by its name only.
ServiceHosts loadServiceHosts(const std::string &dataDir, std::string &warning);

} // namespace sfcop::app
