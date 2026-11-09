#include "Directory.h"

namespace sfcop::directory {

const char* const kFirewallPath = "/TitanServers/Firewall";
const char* const kFirewallDetectorName = "TitanFirewallDetector";

std::vector<ServiceEntity> entitiesFor(const DirectoryConfig& config, const std::string& path,
                                       std::int16_t& status) {
    std::vector<ServiceEntity> out;
    status = 0;
    if (path == kFirewallPath) {
        ServiceEntity e;
        e.name = kFirewallDetectorName;
        e.displayName = kFirewallDetectorName;
        e.address = config.detectorAddress;
        e.port = config.detectorPort;
        out.push_back(e);
        return out;
    }
    if (path == config.campaignPath) {
        for (const CampaignEntry& c : config.campaigns) {
            ServiceEntity e;
            e.name = c.info.campaignName;
            e.displayName = c.info.campaignName;
            e.address = c.address;
            e.port = c.port;
            e.date = c.date;
            e.objects.push_back(DataObject{kCampaignInfoType, encodeCampaignInfo(c.info)});
            out.push_back(e);
        }
        return out;
    }
    status = kDirNotFound;
    return out;
}

void GetDirSession::onBytes(const std::uint8_t* data, std::size_t size) {
    if (done_) return;  // one request per connection; anything after it is ignored
    buffer_.insert(buffer_.end(), data, data + size);
    if (buffer_.size() < 4) return;
    std::uint32_t length = std::uint32_t(buffer_[0]) | std::uint32_t(buffer_[1]) << 8 |
                           std::uint32_t(buffer_[2]) << 16 | std::uint32_t(buffer_[3]) << 24;
    if (length <= 4 || length >= kMaxFrame) {
        log_.debug("getdir: bad frame length; closed");
        done_ = true;
        close();
        return;
    }
    if (buffer_.size() < length) return;
    Bytes frame(buffer_.begin(), buffer_.begin() + std::ptrdiff_t(length));
    done_ = true;
    GetDirRequest request;
    if (!decodeGetDirRequest(frame, request)) {
        log_.debug("getdir: malformed request; closed");
        close();
        return;
    }
    std::int16_t status = 0;
    std::vector<ServiceEntity> entities = entitiesFor(config_, request.path, status);
    Bytes reply = encodeGetDirReply(status, request.flags, entities);
    send(std::string(reply.begin(), reply.end()));
    log_.info("getdir: status " + std::to_string(status) + ", " + std::to_string(entities.size()) +
              " entries");
    close();
}

void DetectSession::onBytes(const std::uint8_t* data, std::size_t size) {
    if (done_) return;
    buffer_.insert(buffer_.end(), data, data + size);
    if (buffer_.size() < 2) return;
    std::size_t length = std::size_t(buffer_[0] | (buffer_[1] << 8));
    if (length <= 2 || length > 256) {
        log_.debug("detect: bad frame length; closed");
        done_ = true;
        close();
        return;
    }
    if (buffer_.size() < length) return;
    Bytes frame(buffer_.begin(), buffer_.begin() + std::ptrdiff_t(length));
    done_ = true;
    DetectRequest request;
    if (!decodeDetectRequest(frame, request)) {
        log_.debug("detect: malformed request; closed");
        close();
        return;
    }
    // The client cannot tell whether a connection came back, so success is answered at once.
    Bytes reply = encodeDetectReply(0);
    send(std::string(reply.begin(), reply.end()));
    log_.info("detect: answered success");
    close();
}

}  // namespace sfcop::directory
