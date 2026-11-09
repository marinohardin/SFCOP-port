// The directory's two sessions, one per TCP connection (src/net/DIRECTORY_API.md):
//  - GetDirSession answers the client's directory queries: the firewall detector and the
//    campaign servers.
//  - DetectSession answers the firewall detection request with success, without connecting back.
// Both take bytes and give bytes; the TcpServer (or a test) carries them.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Campaigns.h"
#include "Log.h"
#include "TcpServer.h"
#include "Wire.h"

namespace sfcop::directory {

// The literal values the client looks for (DIRECTORY_API.md, "What the client takes").
extern const char* const kFirewallPath;          // "/TitanServers/Firewall"
extern const char* const kFirewallDetectorName;  // "TitanFirewallDetector"

struct DirectoryConfig {
    std::string campaignPath = "/StarFleetCommand2/Game/Release";
    std::uint32_t detectorAddress = 0;  // public IPv4, host order, as clients reach it
    std::uint16_t detectorPort = 15102;
    std::vector<CampaignEntry> campaigns;
};

// The entities and status for one request path.
std::vector<ServiceEntity> entitiesFor(const DirectoryConfig& config, const std::string& path,
                                       std::int16_t& status);

class GetDirSession : public StreamSession {
public:
    GetDirSession(const DirectoryConfig& config, const Logger& log) : config_(config), log_(log) {}
    void onBytes(const std::uint8_t* data, std::size_t size) override;

private:
    const DirectoryConfig& config_;
    const Logger& log_;
    Bytes buffer_;
    bool done_ = false;
};

class DetectSession : public StreamSession {
public:
    explicit DetectSession(const Logger& log) : log_(log) {}
    void onBytes(const std::uint8_t* data, std::size_t size) override;

private:
    const Logger& log_;
    Bytes buffer_;
    bool done_ = false;
};

}  // namespace sfcop::directory
