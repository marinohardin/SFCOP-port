// sfcop-directory: the stand-in for the original directory and its firewall detector
// (src/net/DIRECTORY_API.md, "Directory" and "Firewall detection").
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>

#include "Campaigns.h"
#include "Config.h"
#include "Directory.h"
#include "Health.h"
#include "TcpServer.h"

using namespace sfcop::directory;

namespace {

volatile std::sig_atomic_t gStop = 0;

void writeLine(LogLevel level, const std::string& sentence) {
    std::time_t t = std::time(nullptr);
    char stamp[32];
    std::tm utc{};
    gmtime_r(&t, &utc);
    std::strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &utc);
    std::fprintf(stderr, "%s %s directory: %s\n", stamp, levelName(level), sentence.c_str());
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
        std::fprintf(stderr, "sfcop-directory: %s\n", error.c_str());
        return 2;
    }
    if (options.help) {
        std::printf(
            "sfcop-directory [--listen ADDR:PORT] [--detector-listen ADDR:PORT]\n"
            "                --public-address ADDR [--campaigns FILE] [--campaign-path PATH]\n"
            "                [--health-listen ADDR:PORT] [--log-level debug|info|warn|error]\n"
            "Each option overrides its variable: SFCOP_DIRECTORY_LISTEN, SFCOP_DETECTOR_LISTEN,\n"
            "SFCOP_PUBLIC_ADDRESS, SFCOP_CAMPAIGN_FILE, SFCOP_CAMPAIGN_PATH, SFCOP_HEALTH_LISTEN,\n"
            "SFCOP_LOG_LEVEL.\n");
        return 0;
    }
    Logger log(options.logLevel, writeLine);
    if (options.publicAddress.empty()) {
        log.error("SFCOP_PUBLIC_ADDRESS is not set: the address clients reach this host at");
        return 2;
    }

    DirectoryConfig config;
    config.campaignPath = options.campaignPath;
    parseDottedQuad(options.publicAddress, config.detectorAddress);
    config.detectorPort = options.detectorListen.port;
    {
        std::ifstream in(options.campaignFile);
        if (!in) {
            log.error("cannot read the campaign file " + options.campaignFile);
            return 2;
        }
        std::stringstream text;
        text << in.rdbuf();
        if (!parseCampaigns(text.str(), config.campaigns, error)) {
            log.error("campaign file: " + error);
            return 2;
        }
    }

    TcpServer directory([&](std::uint32_t) -> std::unique_ptr<StreamSession> {
        return std::make_unique<GetDirSession>(config, log);
    });
    TcpServer detector([&](std::uint32_t) -> std::unique_ptr<StreamSession> {
        return std::make_unique<DetectSession>(log);
    });
    if (!directory.listen(options.listen.address, options.listen.port, error) ||
        !detector.listen(options.detectorListen.address, options.detectorListen.port, error)) {
        log.error(error);
        return 1;
    }
    HealthState health;
    health.entries = config.campaigns.size();
    HealthListener healthListener;
    if (!healthListener.start(options.health.address, options.health.port, health)) {
        log.error("cannot listen for the health endpoint");
        return 1;
    }

    std::signal(SIGTERM, [](int) { gStop = 1; });
    std::signal(SIGINT, [](int) { gStop = 1; });
    std::signal(SIGPIPE, SIG_IGN);
    std::uint64_t start = nowMs();
    log.info("started: " + std::to_string(config.campaigns.size()) + " campaign entries");
    while (!gStop) {
        directory.poll(50, nowMs());
        detector.poll(50, nowMs());
        health.uptimeSeconds = (nowMs() - start) / 1000;
    }
    healthListener.stop();
    log.info("stopped");
    return 0;
}
