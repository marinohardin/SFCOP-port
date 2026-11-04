#include "hle/dplay/Lobby.h"

#include <cstring>

namespace sfcop::hle::dplay {

HResult copyOut(const std::vector<std::uint8_t> &bytes, std::uint8_t *dest, std::uint32_t *size) {
    if (size == nullptr) {
        return kDpErrInvalidParams;
    }
    std::uint32_t needed = static_cast<std::uint32_t>(bytes.size());
    if (dest == nullptr || *size < needed) {
        *size = needed;
        return kDpErrBufferTooSmall;
    }
    if (needed != 0) {
        std::memcpy(dest, bytes.data(), needed);
    }
    *size = needed;
    return kDpOk;
}

std::uint32_t Lobby::AddRef() { return ++refs_; }

std::uint32_t Lobby::Release() { return --refs_; }

HResult Lobby::EnumAddress(const std::uint8_t *address, std::size_t bytes,
                           const std::function<bool(const AddressElement &)> &callback) {
    if (!callback) {
        return kDpErrInvalidParams;
    }
    std::optional<std::vector<AddressElement>> elements = parseAddress(address, bytes);
    if (!elements) {
        return kDpErrInvalidParams;
    }
    for (const AddressElement &e : *elements) {
        if (!callback(e)) {
            break;
        }
    }
    return kDpOk;
}

HResult Lobby::CreateAddress(const Guid &provider, const Guid &type, const std::uint8_t *data,
                             std::size_t bytes, std::vector<std::uint8_t> *out) {
    if (out == nullptr || (data == nullptr && bytes != 0)) {
        return kDpErrInvalidParams;
    }
    std::vector<std::uint8_t> sp(provider.bytes.begin(), provider.bytes.end());
    std::vector<std::uint8_t> chunk;
    if (bytes != 0) {
        chunk.assign(data, data + bytes);
    }
    *out = buildAddress({{kAidServiceProvider, sp}, {type, chunk}});
    return kDpOk;
}

HResult Lobby::CreateCompoundAddress(const std::vector<AddressElement> &elements,
                                     std::vector<std::uint8_t> *out) {
    if (out == nullptr || elements.empty()) {
        return kDpErrInvalidParams;
    }
    *out = buildAddress(elements);
    return kDpOk;
}

HResult Lobby::GetConnectionSettings() { return kDpErrNotLobbied; }
HResult Lobby::SetConnectionSettings() { return kDpErrNotLobbied; }
HResult Lobby::Connect() { return kDpErrNotLobbied; }
HResult Lobby::ConnectEx() { return kDpErrNotLobbied; }
HResult Lobby::WaitForConnectionSettings() { return kDpErrNotLobbied; }
HResult Lobby::RunApplication() { return kDpErrUnsupported; }

} // namespace sfcop::hle::dplay
