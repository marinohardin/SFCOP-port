#include <doctest/doctest.h>

#include <vector>

#include "Campaigns.h"
#include "Directory.h"
#include "TestSupport.h"

using namespace sfcop::directory;
using sfcop::directory::test::hex;
using sfcop::directory::test::text;
using sfcop::directory::test::utf16;

namespace {

struct Captured {
    std::vector<std::string> lines;
    Logger logger{LogLevel::kDebug,
                  [this](LogLevel, const std::string& s) { lines.push_back(s); }};
};

DirectoryConfig testConfig() {
    DirectoryConfig c;
    c.detectorAddress = 0xC0000214;  // 192.0.2.20
    c.detectorPort = 2021;
    CampaignEntry e;
    e.address = 0xC000020A;  // 192.0.2.10
    e.port = 27000;
    e.date = 0x3F000000;
    e.info.id = 1;
    e.info.campaignName = "TestCampaign";
    c.campaigns.push_back(e);
    return c;
}

const char* const kFirewallQuery = "3f 00 00 00 05 02 00 67 00 37 08 02 06 00 00 16 00";

}  // namespace

TEST_CASE("a firewall query is answered with the detector and the connection closes") {
    DirectoryConfig config = testConfig();
    Captured log;
    GetDirSession s(config, log.logger);
    Bytes q = hex(std::string(kFirewallQuery) + utf16("/TitanServers/Firewall") + "00 00");
    // in two pieces, split inside the length field
    s.onBytes(q.data(), 2);
    CHECK(s.takeOutput().empty());
    s.onBytes(q.data() + 2, q.size() - 2);
    CHECK(text(hex("74 00 00 00 05 02 00 03 00 00 00 80 37 08 02 06 01 00 53 15 00" +
                   utf16("TitanFirewallDetector") + "06 07 e5 c0 00 02 14 15 00" +
                   utf16("TitanFirewallDetector") + "00 00")) == s.takeOutput());
    CHECK(s.closing());
}

TEST_CASE("the campaign query lists every configured campaign server") {
    DirectoryConfig config = testConfig();
    Captured log;
    GetDirSession s(config, log.logger);
    Bytes q = encodeGetDirRequest(GetDirRequest{0x06020E77u, 0, config.campaignPath, {}});
    s.onBytes(q.data(), q.size());
    std::string out = s.takeOutput();
    REQUIRE(out.size() > 18);
    // status 0, last, the request's flags, one entity, type 'S', its name
    CHECK(out.substr(4, 16) == text(hex("05 02 00 03 00 00 00 80 77 0e 02 06 01 00 53 0c")));
    CHECK(out.find("class tCampaignInfo") != std::string::npos);
    CHECK(s.closing());
}

TEST_CASE("an unknown path gets directory-not-found") {
    DirectoryConfig config = testConfig();
    Captured log;
    GetDirSession s(config, log.logger);
    Bytes q = encodeGetDirRequest(GetDirRequest{0x06020E77u, 0, "/Other", {}});
    s.onBytes(q.data(), q.size());
    CHECK(s.takeOutput() == text(hex("12 00 00 00 05 02 00 03 00 50 fb 80 77 0e 02 06 00 00")));
}

TEST_CASE("a malformed or oversized frame closes without a reply") {
    DirectoryConfig config = testConfig();
    Captured log;
    GetDirSession a(config, log.logger);
    Bytes junk = hex("08 00 00 00 05 02 00 99");
    a.onBytes(junk.data(), junk.size());
    CHECK(a.takeOutput().empty());
    CHECK(a.closing());

    GetDirSession b(config, log.logger);
    Bytes huge = hex("00 e8 03 00");  // 0x3E800: one past the client's own limit
    b.onBytes(huge.data(), huge.size());
    CHECK(b.takeOutput().empty());
    CHECK(b.closing());
}

TEST_CASE("the detector answers success at once and logs no address") {
    Captured log;
    DetectSession s(log.logger);
    Bytes q = hex("0f 00 05 03 00 01 00 fc 08 01 20 4e 00 00 00");
    s.onBytes(q.data(), q.size());
    CHECK(s.takeOutput() == text(hex("09 00 05 03 00 02 00 00 00")));
    CHECK(s.closing());
    for (const std::string& line : log.lines) CHECK(line.find("192.0.2") == std::string::npos);
}

TEST_CASE("the campaign file fills tCampaignInfo fields by their original names") {
    std::vector<CampaignEntry> out;
    std::string error;
    REQUIRE(parseCampaigns(
        "# a test server\n"
        "[campaign]\n"
        "address = 192.0.2.10:27000\n"
        "date = 1056964608\n"
        "ID = 7\n"
        "CampaignName = TestCampaign\n"
        "MissionList = one, two\n"
        "RaceList = 1, 2, 3\n"
        "PlayersMax = 40\n"
        "CurrentTime = 1,2,3,4,5,6,7,8,9\n"
        "[campaign]\n"
        "address = 198.51.100.4:27001\n",
        out, error));
    REQUIRE(out.size() == 2);
    CHECK(out[0].address == 0xC000020Au);
    CHECK(out[0].port == 27000);
    CHECK(out[0].date == 0x3F000000u);
    CHECK(out[0].info.id == 7);
    CHECK(out[0].info.campaignName == "TestCampaign");
    CHECK(out[0].info.missionList == std::vector<std::string>{"one", "two"});
    CHECK(out[0].info.raceList == std::vector<std::uint32_t>{1, 2, 3});
    CHECK(out[0].info.playersMax == 40);
    CHECK(out[0].info.currentTime[8] == 9);
    CHECK(out[0].info.serverVersion == "2.5.5.2");
    CHECK(out[1].port == 27001);
}

TEST_CASE("a campaign file with a typo, a bad number or no address is refused whole") {
    std::vector<CampaignEntry> out;
    std::string error;
    CHECK_FALSE(parseCampaigns("[campaign]\naddress = 192.0.2.10:27000\nCampaignNmae = x\n", out,
                               error));
    CHECK(error.find("line 3") != std::string::npos);
    CHECK_FALSE(parseCampaigns("[campaign]\naddress = 192.0.2.10:27000\nPlayersMax = many\n", out,
                               error));
    CHECK_FALSE(parseCampaigns("[campaign]\nCampaignName = x\n", out, error));
    CHECK_FALSE(parseCampaigns("CampaignName = x\n", out, error));
    CHECK_FALSE(parseCampaigns("[campaign]\naddress = 192.0.2.10\n", out, error));
    CHECK(out.empty());
}
