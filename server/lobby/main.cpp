// sfcop-lobby: the lobby API (server/README.md); the UDP rendezvous and relay are sfcop-relay.
#include "Config.h"
#include "Log.h"
#include "Server.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>

namespace {
sfcop::lobby::Server* gServer = nullptr;
void onSignal(int) {
    if (gServer) gServer->requestStop();
}
} // namespace

int main(int argc, char**) {
    if (argc > 1) {
        std::fprintf(stderr, "usage: sfcop-lobby   (configured by environment variables; see server/README.md)\n");
        return 2;
    }
    std::string error;
    auto config = sfcop::lobby::loadConfig([](const char* n) { return std::getenv(n); }, error);
    if (!config) {
        std::fprintf(stderr, "sfcop-lobby: %s\n", error.c_str());
        return 2;
    }
    sfcop::lobby::Server server(*config);
    if (!server.start(error)) {
        std::fprintf(stderr, "sfcop-lobby: %s\n", error.c_str());
        return 1;
    }
    gServer = &server;
    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);
    std::signal(SIGPIPE, SIG_IGN);
    server.run();
    return 0;
}
