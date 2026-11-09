// The directory's messages as the original client sends and reads them (src/net/DIRECTORY_API.md,
// "Directory" and "Firewall detection"). Encoders produce whole frames, length prefix included;
// decoders take a whole frame and check every length against the bytes they were given.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sfcop::directory {

using Bytes = std::vector<std::uint8_t>;

// Little-endian writer for the original's encodings.
class Writer {
public:
    void u8(std::uint8_t v) { out_.push_back(v); }
    void u16(std::uint16_t v);
    void u32(std::uint32_t v);
    void raw(const Bytes& b) { out_.insert(out_.end(), b.begin(), b.end()); }
    void raw(const std::string& s) { out_.insert(out_.end(), s.begin(), s.end()); }
    void wstr(const std::string& ascii);  // u16 count, UTF-16LE; the text is ASCII
    void str8(const std::string& s);      // u8 length (at most 127)
    void blob16(const Bytes& b);          // u16 length (at most 32767)
    void dstr(const std::string& s);      // u32 length
    const Bytes& bytes() const { return out_; }
    Bytes take() { return std::move(out_); }

private:
    Bytes out_;
};

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
    bool u8(std::uint8_t& v);
    bool u16(std::uint16_t& v);
    bool u32(std::uint32_t& v);
    // A wstr whose code units are all below 0x80, as ASCII; false for anything else.
    bool wstr(std::string& ascii);
    bool str8(std::string& s);
    std::size_t remaining() const { return size_ - pos_; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
};

// The CRC the original's object streams carry: MSB-first CRC-32, polynomial 0x04C11DB7, initial
// value 0xFFFFFFFF, no reflection, no final XOR.
std::uint32_t streamCrc(const std::string& text);

// Frame limits of the client's reader (STARFLEETOP 0x006D1030): the length counts itself and is
// below 0x3E800.
constexpr std::uint32_t kMaxFrame = 0x3E800;

constexpr std::uint16_t kServiceDirectory = 2;
constexpr std::uint16_t kGetDirRequest = 0x0067;
constexpr std::uint16_t kGetDirReply = 0x0003;
constexpr std::int16_t kDirNotFound = -1200;

// Get flags (DIRECTORY_API.md, "GetDir reply"), our names.
namespace getflag {
constexpr std::uint32_t kType = 0x10;
constexpr std::uint32_t kDisplayName = 0x20;
constexpr std::uint32_t kDate = 0x40;
constexpr std::uint32_t kUnknown80 = 0x80;
constexpr std::uint32_t kUnknown100 = 0x100;
constexpr std::uint32_t kDataType = 0x200;
constexpr std::uint32_t kDataPayload = 0x400;
constexpr std::uint32_t kDataObjects = 0x800;
constexpr std::uint32_t kUnknown2000 = 0x2000;
constexpr std::uint32_t kDirPath = 0x10000;
constexpr std::uint32_t kDirName = 0x20000;
constexpr std::uint32_t kDirBool = 0x40000;
constexpr std::uint32_t kServicePath = 0x01000000;
constexpr std::uint32_t kServiceName = 0x02000000;
constexpr std::uint32_t kServiceAddress = 0x04000000;
}  // namespace getflag

struct GetDirRequest {
    std::uint32_t flags = 0;
    std::uint16_t unknown = 0;
    std::string path;
    std::vector<std::string> dataTypes;
};

// One frame (length included) -> the request. False for any other header or a short field.
bool decodeGetDirRequest(const Bytes& frame, GetDirRequest& out);
Bytes encodeGetDirRequest(const GetDirRequest& request);  // what the client sends; for tests

struct DataObject {
    std::string type;
    Bytes payload;
};

// A service entity ('S'). Only the fields the reply's flags select are written.
struct ServiceEntity {
    std::string path;
    std::string name;
    std::string displayName;
    std::uint32_t address = 0;  // IPv4, host order
    std::uint16_t port = 0;
    std::uint32_t date = 0;
    std::vector<DataObject> objects;
};

// The whole reply frame: status, sequence byte 0x80 (last), the flags, the entities.
Bytes encodeGetDirReply(std::int16_t status, std::uint32_t flags,
                        const std::vector<ServiceEntity>& entities);

// tCampaignInfo, version 3 (DIRECTORY_API.md, "tCampaignInfo"); field names are the original's.
struct CampaignInfo {
    std::uint32_t id = 0;
    std::uint32_t lockId = 0;
    std::string templateName, description, earlyMapName, midMapName, lateMapName;
    std::vector<std::string> missionList;
    std::vector<std::uint32_t> raceList;
    std::uint32_t campaignEra = 0, campaignDifficulty = 0, triggerPrestige = 0;
    std::string triggerMission, campaignName, characterName;
    std::uint32_t characterRace = 0;
    std::string serverPassword;
    std::uint32_t currentTime[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::uint32_t playersCurrent = 0, playersMax = 0;
    std::uint32_t playersLoggedOnCurrent = 0, playersLoggedOnMax = 0;
    std::string serverVersion = "2.5.5.2", validClientVersion = "2.5.5.2";
};

// The data object's type string and payload.
extern const char* const kCampaignInfoType;  // "class tCampaignInfo"
Bytes encodeCampaignInfo(const CampaignInfo& info);

// Firewall detection: 2-byte length prefix, header 5, service 3.
struct DetectRequest {
    std::uint16_t port = 0;
    std::uint8_t flagA = 0;
    std::uint32_t waitMs = 0;
    std::uint8_t flagB = 0;
};
bool decodeDetectRequest(const Bytes& frame, DetectRequest& out);
Bytes encodeDetectRequest(const DetectRequest& request);  // for tests
Bytes encodeDetectReply(std::int16_t status);

}  // namespace sfcop::directory
