// sfcop-relay: the UDP rendezvous and relay service (DESIGN §6.8, src/net/RENDEZVOUS.md).
#include <csignal>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <random>
#include <sstream>

#include "Config.h"
#include "Health.h"
#include "RelayService.h"
#include "UdpSocket.h"

using namespace sfcop::relay;

namespace {

volatile std::sig_atomic_t gStop = 0;
volatile std::sig_atomic_t gReload = 0;

class SystemTime : public TimeSource {
public:
    SystemTime() : start_(std::chrono::steady_clock::now()) {}
    std::uint64_t nowMs() const override {
        return std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start_).count());
    }
    std::uint32_t unixSeconds() const override { return std::uint32_t(std::time(nullptr)); }

private:
    std::chrono::steady_clock::time_point start_;
};

void writeLine(LogLevel level, const std::string& sentence) {
    std::time_t t = std::time(nullptr);
    char stamp[32];
    std::tm utc{};
    gmtime_r(&t, &utc);
    std::strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &utc);
    std::fprintf(stderr, "%s %s relay: %s\n", stamp, levelName(level), sentence.c_str());
    std::fflush(stderr);
}

bool loadBlocklist(const std::string& path, std::unordered_set<std::uint32_t>& out, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot read the block list";
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    return parseBlocklist(text.str(), out, error);
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    std::string error;
    if (!applyEnvironment(options, [](const char* n) -> const char* { return std::getenv(n); }, error) || !parseArgs(argc, argv, options, error)) {
        std::fprintf(stderr, "sfcop-relay: %s\n", error.c_str());
        return 2;
    }
    if (options.help) {
        std::printf(
            "sfcop-relay [--port N] [--health-port N] [--bind ADDR] [--health-bind ADDR]\n"
            "            [--blocklist FILE] [--log-level debug|info|warn|error]\n"
            "The ticket key is read from SFCOP_TICKET_KEY (64 hexadecimal digits); the UDP port, the loopback\n"
            "HTTP listener and the log level from SFCOP_RELAY_PORT, SFCOP_HEALTH_LISTEN and\n"
            "SFCOP_LOG_LEVEL when set (options override them).\n");
        return 0;
    }
    Logger log(options.logLevel, writeLine);

    const char* keyText = std::getenv("SFCOP_TICKET_KEY");
    RelayConfig config;
    if (!keyText || !parseHexKey(keyText, config.ticketKey)) {
        log.error("SFCOP_TICKET_KEY is missing or is not 64 hexadecimal digits");
        return 2;
    }

    std::unordered_set<std::uint32_t> blocklist;
    if (!options.blocklistPath.empty() && !loadBlocklist(options.blocklistPath, blocklist, error)) {
        log.error("block list: " + error);
        return 2;
    }

    UdpSocket socket;
    if (!socket.open(options.bindAddress, options.port, error)) {
        log.error(error);
        return 1;
    }
    AdminState admin;
    HttpListener http;
    if (!http.start(options.healthAddress, options.healthPort, admin)) {
        log.error("cannot listen for the health endpoint");
        return 1;
    }

    std::signal(SIGTERM, [](int) { gStop = 1; });
    std::signal(SIGINT, [](int) { gStop = 1; });
    std::signal(SIGHUP, [](int) { gReload = 1; });

    SystemTime time;
    std::random_device device;
    std::mt19937 engine(device());
    RelayService service(config, socket, time, log, [&engine] { return std::uint32_t(engine()); });
    service.setBlocklist(blocklist);
    log.info("started");

    std::uint8_t buffer[2048];
    std::uint64_t lastTick = 0;
    while (!gStop) {
        Endpoint from;
        int n = socket.receive(from, buffer, sizeof buffer, 20);
        if (n < 0) {
            log.error("the UDP socket failed");
            break;
        }
        // Drain what is already waiting before the housekeeping.
        for (int burst = 0; n > 0 && burst < 256; ++burst) {
            service.onDatagram(from, buffer, std::size_t(n));
            n = socket.receive(from, buffer, sizeof buffer, 0);
        }
        std::uint64_t now = time.nowMs();
        if (now - lastTick >= 20) {
            lastTick = now;
            for (std::uint64_t session : admin.takeRemovals()) service.removeSession(session);
            if (gReload) {
                gReload = 0;
                std::unordered_set<std::uint32_t> fresh;
                if (options.blocklistPath.empty()) {
                    service.setBlocklist(fresh);
                } else if (loadBlocklist(options.blocklistPath, fresh, error)) {
                    service.setBlocklist(fresh);
                    log.info("block list reloaded");
                } else {
                    log.warn("block list not reloaded: " + error);
                }
            }
            service.tick();
            admin.bindings = service.bindingCount();
            admin.uptimeSeconds = now / 1000;
        }
    }
    http.stop();
    log.info("stopped");
    return 0;
}
