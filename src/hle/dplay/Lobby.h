// IDirectPlayLobby3A (DESIGN §6.1): the address methods the game calls to build the TCP/IP
// connection it hands IDirectPlay4A::InitializeConnection, and the lobby-launch methods, which
// answer DPERR_NOTLOBBIED: a native client is never started by a DirectPlay lobby
// application (the lobby service of DESIGN §5.12 is Phase 3 and does not use this interface).
//
// Lifetime: a COM reference count, as DirectPlay.h says.
#pragma once

#include "hle/dplay/Address.h"
#include "hle/dplay/DplayDefs.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace sfcop::hle::dplay {

// The size-query convention of the address and settings methods: if `dest` is null or `*size`
// is less than `bytes` needs, `*size` becomes that size and the result is
// DPERR_BUFFERTOOSMALL; otherwise the bytes are copied and `*size` is their size.
HResult copyOut(const std::vector<std::uint8_t> &bytes, std::uint8_t *dest, std::uint32_t *size);

class Lobby {
public:
    std::uint32_t AddRef();
    std::uint32_t Release();

    // Calls `callback` for every chunk of `address`, DPAID_TotalSize first, until it returns
    // false. DPERR_INVALIDPARAMS for a malformed address.
    HResult EnumAddress(const std::uint8_t *address, std::size_t bytes,
                        const std::function<bool(const AddressElement &)> &callback);
    // The address of one data chunk under service provider `provider`.
    HResult CreateAddress(const Guid &provider, const Guid &type, const std::uint8_t *data,
                          std::size_t bytes, std::vector<std::uint8_t> *out);
    // The address made of `elements`, in order. DPERR_INVALIDPARAMS for none.
    HResult CreateCompoundAddress(const std::vector<AddressElement> &elements,
                                  std::vector<std::uint8_t> *out);

    // The game was not started by a lobby application: DPERR_NOTLOBBIED.
    HResult GetConnectionSettings();
    HResult SetConnectionSettings();
    HResult Connect();
    HResult ConnectEx();
    HResult WaitForConnectionSettings();
    // Starting another application through a lobby: DPERR_UNSUPPORTED.
    HResult RunApplication();

private:
    std::atomic<std::uint32_t> refs_{1};
};

} // namespace sfcop::hle::dplay
