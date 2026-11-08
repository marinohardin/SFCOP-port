// sfcop_nettest: the Rendezvous test harness (README.md).
//
//   sfcop_nettest pair [cone|symmetric|blocked|forced|all]
//       Two processes on this machine, each behind a simulated NAT (NetPair.h); prints the path
//       each scenario took and how long ConnectToPeer() took. Exit 0 when every scenario took
//       the expected path in time.
//   sfcop_nettest serve --port <n> --key <64 hex digits>
//       The stand-in rendezvous and relay on UDP port <n> of every interface, until interrupted.
//   sfcop_nettest host --service <host[:port]> --key <hex> [--session <16 hex>] [--wait <s>]
//   sfcop_nettest join --service <host[:port]> --key <hex> [--session <16 hex>] [--force-relay]
//       The two ends of a real-network test: the host binds as peer 1 and waits for the joiner;
//       the joiner binds as peer 2, reaches the host, and prints "<path> <ms>".
//
// The key is the test key shared by the three machines, from which the stand-in service checks
// tickets and the clients issue their own; it is a test value, never the lobby's real key.
#include "NetPair.h"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using namespace sfcop;

volatile std::sig_atomic_t gStop = 0;

void onSignal(int) {
    gStop = 1;
}

int usage() {
    std::fprintf(stderr, "usage: sfcop_nettest pair [cone|symmetric|blocked|forced|all]\n"
                         "       sfcop_nettest serve --port <n> --key <64 hex>\n"
                         "       sfcop_nettest host --service <host[:port]> --key <64 hex> "
                         "[--session <16 hex>] [--wait <s>]\n"
                         "       sfcop_nettest join --service <host[:port]> --key <64 hex> "
                         "[--session <16 hex>] [--force-relay]\n");
    return 2;
}

const char *option(int argc, char **argv, const char *name) {
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return nullptr;
}

bool flag(int argc, char **argv, const char *name) {
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

bool parseKey(const char *text, nettest::ServiceKey &key) {
    if (!text || std::strlen(text) != 64) {
        return false;
    }
    for (std::size_t i = 0; i < key.size(); ++i) {
        unsigned value = 0;
        if (std::sscanf(text + 2 * i, "%2x", &value) != 1) {
            return false;
        }
        key[i] = static_cast<std::uint8_t>(value);
    }
    return true;
}

int pair(int argc, char **argv) {
    const std::string which = argc > 2 ? argv[2] : "all";
    struct Expected {
        nettest::Scenario scenario;
        const char *path;
        std::uint32_t minMs;
        std::uint32_t maxMs;
    };
    const Expected all[] = {
        {nettest::Scenario::kCone, "direct", 0, 3000},
        {nettest::Scenario::kSymmetric, "relay", 3000, 5000},
        {nettest::Scenario::kBlocked, "relay", 3000, 5000},
        {nettest::Scenario::kForced, "relay", 0, 1500},
    };
    int failures = 0;
    bool ran = false;
    for (const Expected &expected : all) {
        if (which != "all" && which != nettest::scenarioName(expected.scenario)) {
            continue;
        }
        ran = true;
        nettest::PairResult result = nettest::runPair(expected.scenario);
        if (!result.ok) {
            std::printf("%-9s  failed: %s\n", nettest::scenarioName(expected.scenario),
                        result.failure.c_str());
            ++failures;
            continue;
        }
        bool asExpected = result.path == expected.path && result.connectMs >= expected.minMs &&
                          result.connectMs < expected.maxMs;
        std::printf("%-9s  %-6s in %5u ms  (expected %s, %u to %u ms)%s\n",
                    nettest::scenarioName(expected.scenario), result.path.c_str(), result.connectMs,
                    expected.path, expected.minMs, expected.maxMs,
                    asExpected ? "" : "  UNEXPECTED");
        failures += asExpected ? 0 : 1;
    }
    return !ran ? usage() : (failures == 0 ? 0 : 1);
}

int serve(int argc, char **argv) {
    nettest::ServiceKey key{};
    const char *portText = option(argc, argv, "--port");
    if (!portText || !parseKey(option(argc, argv, "--key"), key)) {
        return usage();
    }
    const std::uint16_t port = static_cast<std::uint16_t>(std::strtoul(portText, nullptr, 10));
    std::unique_ptr<platform::Transport> transport = platform::makeTransport();
    platform::Result<platform::SocketId> socket = transport->Open(platform::SocketKind::kUdp);
    if (!socket.Ok() ||
        !transport->SetOption(socket.Value(), platform::SocketOption::kNonBlocking, 1).Ok() ||
        !transport->Bind(socket.Value(), platform::Endpoint{0, port}).Ok()) {
        std::fprintf(stderr, "could not open UDP port %u\n", port);
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    nettest::StandInService service(key);
    std::printf("stand-in rendezvous and relay on UDP port %u; Ctrl-C stops it\n", port);
    std::fflush(stdout);
    const auto start = nettest::detail::Clock::now();
    std::uint32_t lastReport = 0;
    std::uint8_t buffer[2048];
    while (!gStop) {
        platform::SelectSet sets;
        sets.readable.push_back(socket.Value());
        (void)transport->Select(sets, 5);
        const std::uint32_t nowMs = nettest::detail::msSince(start);
        std::vector<nettest::ServiceDatagram> out;
        for (;;) {
            platform::Result<platform::Received> got =
                transport->ReceiveFrom(socket.Value(), buffer, sizeof buffer);
            if (!got.Ok()) {
                break;
            }
            service.OnDatagram(got.Value().from, buffer, got.Value().bytes, nowMs,
                               static_cast<std::uint32_t>(std::time(nullptr)), out);
        }
        service.Tick(nowMs, out);
        for (const nettest::ServiceDatagram &datagram : out) {
            (void)transport->SendTo(socket.Value(), datagram.bytes.data(), datagram.bytes.size(),
                                    datagram.to);
        }
        if (nowMs - lastReport >= 10000) {
            lastReport = nowMs;
            std::printf("bindings %zu, introductions %u, relayed %u, refused %u\n",
                        service.Bindings(), service.Introductions(), service.Relayed(),
                        service.Refused());
            std::fflush(stdout);
        }
    }
    return 0;
}

int client(int argc, char **argv, bool host) {
    nettest::ServiceKey key{};
    net::RendezvousConfig config;
    const char *service = option(argc, argv, "--service");
    if (!service || !net::parseServiceAddress(service, config) ||
        !parseKey(option(argc, argv, "--key"), key)) {
        return usage();
    }
    std::uint64_t session = nettest::detail::kSession;
    if (const char *text = option(argc, argv, "--session")) {
        std::optional<std::uint64_t> parsed = net::parseSessionId(text);
        if (!parsed) {
            return usage();
        }
        session = *parsed;
    }
    config.forceRelay = flag(argc, argv, "--force-relay");
    std::unique_ptr<platform::Transport> transport =
        net::makeRendezvousTransport(platform::makeTransport(), config);
    const std::uint32_t unixNow = static_cast<std::uint32_t>(std::time(nullptr));
    const net::Ticket ticket =
        nettest::issueTicket(key, session, host ? 1 : 2, unixNow + 600, host ? 1 : 2);
    if (host) {
        const char *wait = option(argc, argv, "--wait");
        const std::uint32_t waitMs =
            static_cast<std::uint32_t>((wait ? std::strtoul(wait, nullptr, 10) : 120) * 1000);
        std::printf("host: bound as peer 1 of %s, waiting for the joiner\n",
                    net::formatPeerName({session, 1}).substr(0, 16).c_str());
        std::fflush(stdout);
        int code = nettest::detail::runHost(*transport, ticket, session, waitMs);
        std::printf(code == 0 ? "host: the joiner said hello; answered\n"
                              : "host: failed (code %d)\n",
                    code);
        return code;
    }
    platform::Result<std::vector<std::uint32_t>> addresses = transport->Resolve(config.serviceHost);
    if (!addresses.Ok() || addresses.Value().empty()) {
        std::fprintf(stderr, "join: the service's host name does not resolve\n");
        return 1;
    }
    const platform::Endpoint serviceEndpoint{addresses.Value().front(), config.servicePort};
    std::printf("join: ");
    std::fflush(stdout);
    int code = nettest::detail::runJoiner(*transport, ticket, serviceEndpoint, 1, session);
    std::printf("\n");
    return code;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        return usage();
    }
    const std::string mode = argv[1];
    if (mode == "pair") {
        return pair(argc, argv);
    }
    if (mode == "serve") {
        return serve(argc, argv);
    }
    if (mode == "host" || mode == "join") {
        return client(argc, argv, mode == "host");
    }
    return usage();
}
