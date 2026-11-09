#include "Campaigns.h"

#include <sstream>

namespace sfcop::directory {
namespace {

std::string trim(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return std::string();
    std::size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

bool parseU32(const std::string& text, std::uint32_t& out) {
    if (text.empty() || text.size() > 10) return false;
    std::uint64_t v = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + std::uint64_t(c - '0');
    }
    if (v > 0xFFFFFFFFu) return false;
    out = std::uint32_t(v);
    return true;
}

std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> items;
    if (trim(text).empty()) return items;
    std::stringstream in(text);
    std::string item;
    while (std::getline(in, item, ',')) items.push_back(trim(item));
    return items;
}

bool parseIPv4(const std::string& text, std::uint32_t& out) {
    std::uint32_t value = 0;
    std::size_t i = 0;
    for (int part = 0; part < 4; ++part) {
        std::size_t start = i;
        unsigned v = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9' && i - start < 3) {
            v = v * 10 + unsigned(text[i] - '0');
            ++i;
        }
        if (i == start || v > 255) return false;
        value = (value << 8) | v;
        if (part < 3) {
            if (i >= text.size() || text[i] != '.') return false;
            ++i;
        }
    }
    if (i != text.size()) return false;
    out = value;
    return true;
}

// One `key = value` line into the entry. False for an unknown key or a bad number.
bool setField(CampaignEntry& e, const std::string& key, const std::string& value) {
    CampaignInfo& c = e.info;
    std::string* text = nullptr;
    std::uint32_t* number = nullptr;
    if (key == "address") return parseEndpoint(value, e.address, e.port);
    if (key == "date") number = &e.date;
    else if (key == "ID") number = &c.id;
    else if (key == "LockID") number = &c.lockId;
    else if (key == "TemplateName") text = &c.templateName;
    else if (key == "Description") text = &c.description;
    else if (key == "EarlyMapName") text = &c.earlyMapName;
    else if (key == "MidMapName") text = &c.midMapName;
    else if (key == "LateMapName") text = &c.lateMapName;
    else if (key == "CampaignEra") number = &c.campaignEra;
    else if (key == "CampaignDifficulty") number = &c.campaignDifficulty;
    else if (key == "TriggerPrestige") number = &c.triggerPrestige;
    else if (key == "TriggerMission") text = &c.triggerMission;
    else if (key == "CampaignName") text = &c.campaignName;
    else if (key == "CharacterName") text = &c.characterName;
    else if (key == "CharacterRace") number = &c.characterRace;
    else if (key == "ServerPassword") text = &c.serverPassword;
    else if (key == "PlayersCurrent") number = &c.playersCurrent;
    else if (key == "PlayersMax") number = &c.playersMax;
    else if (key == "PlayersLoggedOnCurrent") number = &c.playersLoggedOnCurrent;
    else if (key == "PlayersLoggedOnMax") number = &c.playersLoggedOnMax;
    else if (key == "ServerVersion") text = &c.serverVersion;
    else if (key == "ValidClientVersion") text = &c.validClientVersion;
    else if (key == "MissionList") {
        c.missionList = splitList(value);
        return true;
    } else if (key == "RaceList") {
        c.raceList.clear();
        for (const std::string& item : splitList(value)) {
            std::uint32_t v = 0;
            if (!parseU32(item, v)) return false;
            c.raceList.push_back(v);
        }
        return true;
    } else if (key == "CurrentTime") {
        std::vector<std::string> items = splitList(value);
        if (items.size() != 9) return false;
        for (std::size_t i = 0; i < 9; ++i) {
            if (!parseU32(items[i], c.currentTime[i])) return false;
        }
        return true;
    }
    if (text) {
        *text = value;
        return true;
    }
    if (number) return parseU32(value, *number);
    return false;
}

}  // namespace

bool parseEndpoint(const std::string& text, std::uint32_t& address, std::uint16_t& port) {
    std::size_t colon = text.rfind(':');
    if (colon == std::string::npos) return false;
    std::uint32_t a = 0, p = 0;
    if (!parseIPv4(text.substr(0, colon), a) || !parseU32(text.substr(colon + 1), p) || p == 0 ||
        p > 65535) {
        return false;
    }
    address = a;
    port = std::uint16_t(p);
    return true;
}

bool parseCampaigns(const std::string& text, std::vector<CampaignEntry>& out, std::string& error) {
    std::vector<CampaignEntry> entries;
    std::vector<bool> hasAddress;
    std::istringstream in(text);
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        std::string where = "line " + std::to_string(number);
        std::size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;
        if (line == "[campaign]") {
            entries.emplace_back();
            hasAddress.push_back(false);
            continue;
        }
        if (entries.empty()) {
            error = where + ": a field before the first [campaign]";
            return false;
        }
        std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            error = where + ": not key = value";
            return false;
        }
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (!setField(entries.back(), key, value)) {
            error = where + ": unknown key or bad value for " + key;
            return false;
        }
        if (key == "address") hasAddress.back() = true;
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!hasAddress[i]) {
            error = "campaign " + std::to_string(i + 1) + " has no address";
            return false;
        }
    }
    out = std::move(entries);
    return true;
}

}  // namespace sfcop::directory
