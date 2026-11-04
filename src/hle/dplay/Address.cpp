#include "hle/dplay/Address.h"

#include "hle/dplay/Bytes.h"

#include <cstdio>

namespace sfcop::hle::dplay {

namespace {

constexpr std::uint32_t kChunkHeaderBytes = 20; // GUID + data size

} // namespace

std::vector<std::uint8_t> buildAddress(const std::vector<AddressElement> &elements) {
    ByteWriter w;
    w.Id(kAidTotalSize);
    w.U32(4);
    w.U32(0); // patched below
    for (const AddressElement &e : elements) {
        if (e.type == kAidTotalSize) {
            continue;
        }
        w.Id(e.type);
        w.U32(static_cast<std::uint32_t>(e.data.size()));
        w.Raw(e.data);
    }
    w.PatchU32(kChunkHeaderBytes, static_cast<std::uint32_t>(w.Size()));
    return w.Take();
}

std::optional<std::vector<AddressElement>> parseAddress(const std::uint8_t *data,
                                                        std::size_t bytes) {
    if (data == nullptr || bytes == 0) {
        return std::nullopt;
    }
    ByteReader r(data, bytes);
    std::vector<AddressElement> out;
    while (r.Ok() && !r.AtEnd()) {
        AddressElement e;
        e.type = r.Id();
        std::uint32_t size = r.U32();
        if (!r.Ok() || size > r.Left()) {
            return std::nullopt;
        }
        e.data = r.Raw(size);
        out.push_back(std::move(e));
    }
    if (!r.Ok()) {
        return std::nullopt;
    }
    return out;
}

std::optional<Guid> serviceProviderOf(const std::vector<AddressElement> &elements) {
    for (const AddressElement &e : elements) {
        if (e.type == kAidServiceProvider && e.data.size() == 16) {
            Guid g;
            for (std::size_t i = 0; i < 16; ++i) {
                g.bytes[i] = e.data[i];
            }
            return g;
        }
    }
    return std::nullopt;
}

TcpIpTarget tcpIpTargetOf(const std::vector<AddressElement> &elements) {
    TcpIpTarget target;
    for (const AddressElement &e : elements) {
        if (e.type == kAidINet) {
            std::string host;
            for (std::uint8_t c : e.data) {
                if (c == 0) {
                    break;
                }
                host.push_back(static_cast<char>(c));
            }
            target.host = host;
        } else if (e.type == kAidINetPort && e.data.size() == 2) {
            target.port = static_cast<std::uint16_t>(e.data[0] | e.data[1] << 8);
        }
    }
    return target;
}

std::vector<std::uint8_t> tcpIpConnection() {
    std::vector<std::uint8_t> sp(kSpTcpIp.bytes.begin(), kSpTcpIp.bytes.end());
    return buildAddress({{kAidServiceProvider, sp}});
}

std::vector<std::uint8_t> tcpIpAddress(std::uint32_t address, std::uint16_t port) {
    std::vector<std::uint8_t> sp(kSpTcpIp.bytes.begin(), kSpTcpIp.bytes.end());
    std::string quad = dottedQuad(address);
    std::vector<std::uint8_t> inet(quad.begin(), quad.end());
    inet.push_back(0);
    std::vector<std::uint8_t> inetPort = {static_cast<std::uint8_t>(port),
                                          static_cast<std::uint8_t>(port >> 8)};
    return buildAddress({{kAidServiceProvider, sp}, {kAidINet, inet}, {kAidINetPort, inetPort}});
}

std::string dottedQuad(std::uint32_t address) {
    char text[16];
    std::snprintf(text, sizeof text, "%u.%u.%u.%u", address >> 24, (address >> 16) & 0xFF,
                  (address >> 8) & 0xFF, address & 0xFF);
    return text;
}

std::optional<std::uint32_t> parseDottedQuad(const std::string &text) {
    std::uint32_t address = 0;
    int parts = 0;
    std::size_t i = 0;
    while (parts < 4) {
        if (i >= text.size() || text[i] < '0' || text[i] > '9') {
            return std::nullopt;
        }
        std::uint32_t part = 0;
        std::size_t digits = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9' && digits < 4) {
            part = part * 10 + static_cast<std::uint32_t>(text[i] - '0');
            ++i;
            ++digits;
        }
        if (part > 255 || digits > 3) {
            return std::nullopt;
        }
        address = address << 8 | part;
        ++parts;
        if (parts < 4) {
            if (i >= text.size() || text[i] != '.') {
                return std::nullopt;
            }
            ++i;
        }
    }
    if (i != text.size()) {
        return std::nullopt;
    }
    return address;
}

} // namespace sfcop::hle::dplay
