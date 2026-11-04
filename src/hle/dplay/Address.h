// DirectPlay addresses: the byte strings EnumConnections hands the game, CreateCompoundAddress
// builds, InitializeConnection takes and GetPlayerAddress returns. An address is a sequence of
// chunks, each a 16-byte data-type GUID, a 32-bit data size and that many bytes; the first chunk
// is DPAID_TotalSize, whose data is the size of the whole address.
#pragma once

#include "hle/dplay/DplayDefs.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sfcop::hle::dplay {

struct AddressElement {
    Guid type;
    std::vector<std::uint8_t> data;

    friend bool operator==(const AddressElement &a, const AddressElement &b) {
        return a.type == b.type && a.data == b.data;
    }
};

// The address made of a DPAID_TotalSize chunk and then `elements`, in order. An element that
// is itself DPAID_TotalSize is left out (the address has exactly one, first).
std::vector<std::uint8_t> buildAddress(const std::vector<AddressElement> &elements);

// Every chunk of an address, DPAID_TotalSize included, in order; std::nullopt if a chunk runs
// past the end or the address is empty.
std::optional<std::vector<AddressElement>> parseAddress(const std::uint8_t *data,
                                                        std::size_t bytes);

// What a TCP/IP address names: a host (a dotted quad or a name; empty for "search the local
// network") and, if the address has DPAID_INetPort, a port.
struct TcpIpTarget {
    std::string host;
    std::optional<std::uint16_t> port;
};

// The service provider of an address (its DPAID_ServiceProvider chunk), if it has one.
std::optional<Guid> serviceProviderOf(const std::vector<AddressElement> &elements);

// The target of a TCP/IP address: its DPAID_INet string (up to its first NUL) and its
// DPAID_INetPort. A chunk of the wrong size is ignored.
TcpIpTarget tcpIpTargetOf(const std::vector<AddressElement> &elements);

// The connection EnumConnections offers: the TCP/IP service provider and nothing else.
std::vector<std::uint8_t> tcpIpConnection();

// The address of a TCP/IP endpoint: service provider, dotted quad and port.
std::vector<std::uint8_t> tcpIpAddress(std::uint32_t address, std::uint16_t port);

// "192.0.2.1" for 0xC0000201.
std::string dottedQuad(std::uint32_t address);
// The address of a dotted quad, or std::nullopt if `text` is not one.
std::optional<std::uint32_t> parseDottedQuad(const std::string &text);

} // namespace sfcop::hle::dplay
