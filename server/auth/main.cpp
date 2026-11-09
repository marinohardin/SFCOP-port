// sfcop-auth: the stand-in for the original account login, its profile search and the version
// check (src/net/DIRECTORY_API.md). Accounts live in this process's memory only.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <random>

#include "Accounts.h"
#include "Config.h"
#include "Health.h"
#include "Sessions.h"
#include "TcpServer.h"

using namespace sfcop::auth;

namespace {

volatile std::sig_atomic_t gStop = 0;

void writeLine(LogLevel level, const std::string& sentence) {
    std::time_t t = std::time(nullptr);
    char stamp[32];
    std::tm utc{};
    gmtime_r(&t, &utc);
    std::strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &utc);
    std::fprintf(stderr, "%s %s auth: %s\n", stamp, levelName(level), sentence.c_str());
    std::fflush(stderr);
}

std::uint64_t nowMs() {
    using namespace std::chrono;
    return std::uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    std::string error;
    if (!applyEnvironment(options, [](const char* n) -> const char* { return std::getenv(n); },
                          error) ||
        !parseArgs(argc, argv, options, error)) {
        std::fprintf(stderr, "sfcop-auth: %s\n", error.c_str());
        return 2;
    }
    if (options.help) {
        std::printf(
            "sfcop-auth [--login-listen ADDR:PORT] [--search-listen ADDR:PORT]\n"
            "           [--patch-listen ADDR:PORT|off] [--health-listen ADDR:PORT]\n"
            "           [--log-level debug|info|warn|error]\n"
            "Each option overrides its variable: SFCOP_LOGIN_LISTEN, SFCOP_SEARCH_LISTEN,\n"
            "SFCOP_PATCH_LISTEN, SFCOP_HEALTH_LISTEN, SFCOP_LOG_LEVEL.\n");
        return 0;
    }
    Logger log(options.logLevel, writeLine);

    std::random_device device;
    std::mt19937 engine(device());
    Random random = [&engine] { return std::uint32_t(engine()); };
    Accounts accounts;

    // The account login stays open after the logon; ten quiet minutes end it.
    TcpServerLimits loginLimits;
    loginLimits.idleMs = 10 * 60 * 1000;
    TcpServer login(
        [&](std::uint32_t) -> std::unique_ptr<StreamSession> {
            return std::make_unique<LoginSession>(accounts, log, random);
        },
        loginLimits);
    TcpServerLimits shortLimits;
    shortLimits.idleMs = 30 * 1000;
    TcpServer search(
        [&](std::uint32_t) -> std::unique_ptr<StreamSession> {
            return std::make_unique<SearchSession>(accounts, log);
        },
        shortLimits);
    TcpServer patch(
        [&](std::uint32_t) -> std::unique_ptr<StreamSession> {
            return std::make_unique<PatchSession>(log);
        },
        shortLimits);
    if (!login.listen(options.login.address, options.login.port, error) ||
        !search.listen(options.search.address, options.search.port, error)) {
        log.error(error);
        return 1;
    }
    // The version check is the one part the client does without: a port it cannot have (another
    // process holds 80) is logged and the service runs on.
    bool patchOn = !options.patchOff;
    if (patchOn && !patch.listen(options.patch.address, options.patch.port, error)) {
        log.warn(error + "; the version check is not served");
        patchOn = false;
    }
    HealthState health;
    HealthListener healthListener;
    if (!healthListener.start(options.health.address, options.health.port, health)) {
        log.error("cannot listen for the health endpoint");
        return 1;
    }

    std::signal(SIGTERM, [](int) { gStop = 1; });
    std::signal(SIGINT, [](int) { gStop = 1; });
    std::signal(SIGPIPE, SIG_IGN);
    std::uint64_t start = nowMs();
    log.info("started");
    while (!gStop) {
        login.poll(30, nowMs());
        search.poll(10, nowMs());
        if (patchOn) patch.poll(10, nowMs());
        health.accounts = accounts.size();
        health.uptimeSeconds = (nowMs() - start) / 1000;
    }
    healthListener.stop();
    log.info("stopped");
    return 0;
}
