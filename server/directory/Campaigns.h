// The campaign servers the directory lists, from a text file the owner writes (README.md,
// "The campaign file"):
//
//   # one block per campaign server
//   [campaign]
//   address = 192.0.2.10:27000
//   CampaignName = TestCampaign
//   MissionList = one, two
//   RaceList = 1, 2
//
// `address` (dotted quad and port) is required; `date` is the entry's u32 date (default 0); every
// other key is a tCampaignInfo field by its original name (Wire.h), and a field left out keeps
// its default. Lists are comma-separated. A key the file does not know fails the whole file, so a
// typo is not served as a default.
#pragma once

#include <string>
#include <vector>

#include "Wire.h"

namespace sfcop::directory {

struct CampaignEntry {
    std::uint32_t address = 0;  // host order
    std::uint16_t port = 0;
    std::uint32_t date = 0;
    CampaignInfo info;
};

bool parseCampaigns(const std::string& text, std::vector<CampaignEntry>& out, std::string& error);

// host:port, a dotted quad and a port 1..65535.
bool parseEndpoint(const std::string& text, std::uint32_t& address, std::uint16_t& port);

}  // namespace sfcop::directory
