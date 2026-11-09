#include "Wire.h"

namespace sfcop::directory {
namespace {

constexpr std::uint8_t kHeaderType = 5;
constexpr std::uint16_t kServiceFirewall = 3;
constexpr std::uint16_t kDetectRequest = 1;
constexpr std::uint16_t kDetectReply = 2;

// Object ids of the nested streams (DIRECTORY_API.md, "tCampaignInfo").
constexpr const char* kCampaignInfoName = ".?AVtCampaignInfo@@";
constexpr const char* kIdName = ".?AVtID@nDatabase@@";
constexpr const char* kCurrentTimeName = ".?AVtCurrentTime@@";

// Puts the length of the whole frame in its first `width` bytes.
Bytes finish(Bytes frame, int width) {
    std::size_t n = frame.size();
    for (int i = 0; i < width; ++i) frame[std::size_t(i)] = std::uint8_t(n >> (8 * i));
    return frame;
}

void header(Writer& w, int lengthWidth, std::uint16_t service, std::uint16_t message) {
    for (int i = 0; i < lengthWidth; ++i) w.u8(0);
    w.u8(kHeaderType);
    w.u16(service);
    w.u16(message);
}

// Checks the length field and the header; leaves `r` after the header.
bool openFrame(const Bytes& frame, int lengthWidth, std::uint16_t service, std::uint16_t message,
               Reader& r) {
    std::uint32_t length = 0;
    for (int i = 0; i < lengthWidth; ++i) {
        std::uint8_t b = 0;
        if (!r.u8(b)) return false;
        length |= std::uint32_t(b) << (8 * i);
    }
    if (length != frame.size() || length <= std::uint32_t(lengthWidth)) return false;
    std::uint8_t type = 0;
    std::uint16_t s = 0, m = 0;
    return r.u8(type) && r.u16(s) && r.u16(m) && type == kHeaderType && s == service &&
           m == message;
}

}  // namespace

const char* const kCampaignInfoType = "class tCampaignInfo";

void Writer::u16(std::uint16_t v) {
    out_.push_back(std::uint8_t(v));
    out_.push_back(std::uint8_t(v >> 8));
}

void Writer::u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out_.push_back(std::uint8_t(v >> (8 * i)));
}

void Writer::wstr(const std::string& ascii) {
    std::size_t n = ascii.size() < 0x7FFF ? ascii.size() : 0x7FFF;
    u16(std::uint16_t(n));
    for (std::size_t i = 0; i < n; ++i) u16(std::uint8_t(ascii[i]));
}

void Writer::str8(const std::string& s) {
    std::size_t n = s.size() < 127 ? s.size() : 127;
    u8(std::uint8_t(n));
    out_.insert(out_.end(), s.begin(), s.begin() + std::ptrdiff_t(n));
}

void Writer::blob16(const Bytes& b) {
    std::size_t n = b.size() < 0x7FFF ? b.size() : 0x7FFF;
    u16(std::uint16_t(n));
    out_.insert(out_.end(), b.begin(), b.begin() + std::ptrdiff_t(n));
}

void Writer::dstr(const std::string& s) {
    u32(std::uint32_t(s.size()));
    raw(s);
}

bool Reader::u8(std::uint8_t& v) {
    if (remaining() < 1) return false;
    v = data_[pos_++];
    return true;
}

bool Reader::u16(std::uint16_t& v) {
    if (remaining() < 2) return false;
    v = std::uint16_t(data_[pos_] | (data_[pos_ + 1] << 8));
    pos_ += 2;
    return true;
}

bool Reader::u32(std::uint32_t& v) {
    if (remaining() < 4) return false;
    v = std::uint32_t(data_[pos_]) | std::uint32_t(data_[pos_ + 1]) << 8 |
        std::uint32_t(data_[pos_ + 2]) << 16 | std::uint32_t(data_[pos_ + 3]) << 24;
    pos_ += 4;
    return true;
}

bool Reader::wstr(std::string& ascii) {
    std::uint16_t n = 0;
    if (!u16(n) || n >= 0x8000 || remaining() < std::size_t(n) * 2) return false;
    ascii.clear();
    for (std::uint16_t i = 0; i < n; ++i) {
        std::uint16_t c = 0;
        u16(c);
        if (c >= 0x80) return false;
        ascii += char(c);
    }
    return true;
}

bool Reader::str8(std::string& s) {
    std::uint8_t n = 0;
    if (!u8(n) || n >= 0x80 || remaining() < n) return false;
    s.assign(reinterpret_cast<const char*>(data_ + pos_), n);
    pos_ += n;
    return true;
}

std::uint32_t streamCrc(const std::string& text) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (char ch : text) {
        crc ^= std::uint32_t(std::uint8_t(ch)) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
        }
    }
    return crc;
}

bool decodeGetDirRequest(const Bytes& frame, GetDirRequest& out) {
    if (frame.size() >= kMaxFrame) return false;
    Reader r(frame.data(), frame.size());
    if (!openFrame(frame, 4, kServiceDirectory, kGetDirRequest, r)) return false;
    GetDirRequest q;
    std::uint16_t count = 0;
    if (!r.u32(q.flags) || !r.u16(q.unknown) || !r.wstr(q.path) || !r.u16(count)) return false;
    for (std::uint16_t i = 0; i < count; ++i) {
        std::string type;
        if (!r.str8(type)) return false;
        q.dataTypes.push_back(type);
    }
    if (r.remaining() != 0) return false;
    out = q;
    return true;
}

Bytes encodeGetDirRequest(const GetDirRequest& q) {
    Writer w;
    header(w, 4, kServiceDirectory, kGetDirRequest);
    w.u32(q.flags);
    w.u16(q.unknown);
    w.wstr(q.path);
    w.u16(std::uint16_t(q.dataTypes.size()));
    for (const std::string& t : q.dataTypes) w.str8(t);
    return finish(w.take(), 4);
}

Bytes encodeGetDirReply(std::int16_t status, std::uint32_t flags,
                        const std::vector<ServiceEntity>& entities) {
    using namespace getflag;
    Writer w;
    header(w, 4, kServiceDirectory, kGetDirReply);
    w.u16(std::uint16_t(status));
    w.u8(0x80);  // the last (and only) message of the reply
    w.u32(flags);
    w.u16(status < 0 ? 0 : std::uint16_t(entities.size()));
    if (status < 0) return finish(w.take(), 4);
    for (const ServiceEntity& e : entities) {
        if (flags & kType) w.u8('S');
        if (flags & kServicePath) w.wstr(e.path);
        if (flags & kServiceName) w.wstr(e.name);
        if (flags & kServiceAddress) {
            w.u8(6);
            w.u8(std::uint8_t(e.port >> 8));  // the port big-endian, then the address in network order
            w.u8(std::uint8_t(e.port));
            for (int i = 3; i >= 0; --i) w.u8(std::uint8_t(e.address >> (8 * i)));
        }
        if (flags & kDisplayName) w.wstr(e.displayName);
        if (flags & kUnknown100) w.u32(0);
        if (flags & kDate) w.u32(e.date);
        if (flags & kUnknown80) w.u32(0);
        if (flags & kUnknown2000) w.u32(0);
        if (flags & kDataObjects) {
            // A data object is sent only when the flags carry both its type and its payload.
            bool full = (flags & kDataType) && (flags & kDataPayload);
            w.u16(full ? std::uint16_t(e.objects.size()) : 0);
            if (full) {
                for (const DataObject& o : e.objects) {
                    w.str8(o.type);
                    w.blob16(o.payload);
                }
            }
        }
    }
    return finish(w.take(), 4);
}

Bytes encodeCampaignInfo(const CampaignInfo& c) {
    Writer w;
    w.u8(3);
    w.u32(streamCrc(kCampaignInfoName));
    w.u8(0);  // the nested tID: version, id, value
    w.u32(streamCrc(kIdName));
    w.u32(c.id);
    w.u32(c.lockId);
    w.dstr(c.templateName);
    w.dstr(c.description);
    w.dstr(c.earlyMapName);
    w.dstr(c.midMapName);
    w.dstr(c.lateMapName);
    w.u32(std::uint32_t(c.missionList.size()));
    for (const std::string& m : c.missionList) w.dstr(m);
    w.u32(std::uint32_t(c.raceList.size()));
    for (std::uint32_t r : c.raceList) w.u32(r);
    w.u32(c.campaignEra);
    w.u32(c.campaignDifficulty);
    w.u32(c.triggerPrestige);
    w.dstr(c.triggerMission);
    w.dstr(c.campaignName);
    w.dstr(c.characterName);
    w.u32(c.characterRace);
    w.dstr(c.serverPassword);
    w.u8(0);  // the nested tCurrentTime: version, id, nine words
    w.u32(streamCrc(kCurrentTimeName));
    for (std::uint32_t v : c.currentTime) w.u32(v);
    w.u32(c.playersCurrent);
    w.u32(c.playersMax);
    w.u32(c.playersLoggedOnCurrent);
    w.u32(c.playersLoggedOnMax);
    w.dstr(c.serverVersion);
    w.dstr(c.validClientVersion);
    return w.take();
}

bool decodeDetectRequest(const Bytes& frame, DetectRequest& out) {
    Reader r(frame.data(), frame.size());
    if (!openFrame(frame, 2, kServiceFirewall, kDetectRequest, r)) return false;
    DetectRequest q;
    if (!r.u16(q.port) || !r.u8(q.flagA) || !r.u32(q.waitMs) || !r.u8(q.flagB) ||
        r.remaining() != 0) {
        return false;
    }
    out = q;
    return true;
}

Bytes encodeDetectRequest(const DetectRequest& q) {
    Writer w;
    header(w, 2, kServiceFirewall, kDetectRequest);
    w.u16(q.port);
    w.u8(q.flagA);
    w.u32(q.waitMs);
    w.u8(q.flagB);
    return finish(w.take(), 2);
}

Bytes encodeDetectReply(std::int16_t status) {
    Writer w;
    header(w, 2, kServiceFirewall, kDetectReply);
    w.u16(std::uint16_t(status));
    return finish(w.take(), 2);
}

}  // namespace sfcop::directory
