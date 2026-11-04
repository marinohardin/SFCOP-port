#include "hle/kernel32rt/VirtualMemory.h"

#include <utility>

namespace sfcop::hle::kernel32rt {

namespace {

constexpr std::uint32_t kPage = 0x1000u;
constexpr std::uint32_t kGranularity = 0x10000u; // where Windows starts a reservation

std::uint32_t pageDown(std::uint32_t address) {
    return address & ~(kPage - 1);
}

std::uint64_t pageUp(std::uint64_t address) {
    return (address + kPage - 1) & ~static_cast<std::uint64_t>(kPage - 1);
}

Answer failed(std::uint32_t error) {
    return Answer{0, error};
}

} // namespace

VirtualMemory::VirtualMemory(Backend backend) : backend_(std::move(backend)) {}

std::map<std::uint32_t, VirtualMemory::Region>::iterator
VirtualMemory::regionOf(std::uint32_t address) {
    auto after = regions_.upper_bound(address);
    if (after == regions_.begin()) {
        return regions_.end();
    }
    auto region = std::prev(after);
    return address - region->first < region->second.size ? region : regions_.end();
}

Answer VirtualMemory::reserve(std::uint32_t address, std::uint32_t size, std::uint32_t type,
                              std::uint32_t protect) {
    const std::uint32_t start = address & ~(kGranularity - 1);
    if (address != 0 && start == 0) {
        // Below 64 KB: the address rounds to zero, which would mean "anywhere" and skip the
        // overlap check below. Windows keeps that range out of reach as well.
        return failed(kErrorInvalidAddress);
    }
    const std::uint64_t end = pageUp(static_cast<std::uint64_t>(address) + size);
    if (end > 0x100000000ull) {
        return failed(kErrorInvalidParameter);
    }
    const auto bytes = static_cast<std::uint32_t>(end - start);
    if (start != 0) {
        auto overlap = regions_.lower_bound(start);
        if ((overlap != regions_.end() && overlap->first < end) || regionOf(start) != regions_.end()) {
            return failed(kErrorInvalidAddress);
        }
    }
    const std::uint32_t base = backend_.reserve(start, bytes);
    if (base == 0) {
        return failed(start != 0 ? kErrorInvalidAddress : kErrorNotEnoughMemory);
    }
    Region region;
    region.size = bytes;
    region.allocationProtect = protect;
    region.pages.assign(bytes / kPage, (type & kMemCommit) != 0 ? protect : 0u);
    regions_[base] = std::move(region);
    return Answer{base, std::nullopt};
}

Answer VirtualMemory::Allocate(std::uint32_t address, std::uint32_t size, std::uint32_t type,
                               std::uint32_t protect) {
    if (size == 0 || (type & (kMemCommit | kMemReserve)) == 0 ||
        (type & ~(kMemCommit | kMemReserve | 0x00100000u /* MEM_TOP_DOWN */)) != 0 ||
        protect == 0) {
        return failed(kErrorInvalidParameter);
    }
    std::lock_guard<std::mutex> hold(lock_);
    if ((type & kMemReserve) != 0 || address == 0) {
        // A commit at no address reserves as well, as Windows does.
        return reserve(address, size, type, protect);
    }
    auto region = regionOf(address);
    const std::uint32_t first = pageDown(address);
    const std::uint64_t end = pageUp(static_cast<std::uint64_t>(address) + size);
    if (region == regions_.end() || end > static_cast<std::uint64_t>(region->first) + region->second.size) {
        return failed(kErrorInvalidAddress);
    }
    for (std::uint64_t page = first; page < end; page += kPage) {
        region->second.pages[(page - region->first) / kPage] = protect;
    }
    return Answer{first, std::nullopt};
}

Answer VirtualMemory::Free(std::uint32_t address, std::uint32_t size, std::uint32_t type) {
    std::lock_guard<std::mutex> hold(lock_);
    auto region = regionOf(address);
    if (region == regions_.end()) {
        return failed(kErrorInvalidParameter);
    }
    if (type == kMemRelease) {
        if (size != 0 || address != region->first) {
            return failed(kErrorInvalidParameter);
        }
        backend_.release(region->first, region->second.size);
        regions_.erase(region);
        return Answer{1, std::nullopt};
    }
    if (type != kMemDecommit) {
        return failed(kErrorInvalidParameter);
    }
    const std::uint32_t first = size == 0 ? region->first : pageDown(address);
    const std::uint64_t regionEnd = static_cast<std::uint64_t>(region->first) + region->second.size;
    const std::uint64_t end =
        size == 0 ? regionEnd : pageUp(static_cast<std::uint64_t>(address) + size);
    if (end > regionEnd) {
        return failed(kErrorInvalidParameter);
    }
    backend_.zero(first, static_cast<std::uint32_t>(end - first));
    for (std::uint64_t page = first; page < end; page += kPage) {
        region->second.pages[(page - region->first) / kPage] = 0;
    }
    return Answer{1, std::nullopt};
}

MemoryInfo VirtualMemory::Query(std::uint32_t address) {
    std::lock_guard<std::mutex> hold(lock_);
    MemoryInfo info;
    info.baseAddress = pageDown(address);
    auto region = regionOf(address);
    if (region == regions_.end()) {
        if (backend_.mapped(info.baseAddress)) {
            info.allocationBase = address & ~(kGranularity - 1);
            info.allocationProtect = kPageReadWrite;
            info.regionSize = kPage;
            info.state = kMemCommit;
            info.protect = kPageReadWrite;
            info.type = kMemPrivate;
        } else {
            info.regionSize = kPage;
            info.state = kMemFree;
            info.protect = kPageNoAccess;
        }
        return info;
    }
    const Region &r = region->second;
    std::size_t index = (info.baseAddress - region->first) / kPage;
    const std::uint32_t protection = r.pages[index];
    std::size_t last = index;
    while (last + 1 < r.pages.size() && r.pages[last + 1] == protection) {
        ++last;
    }
    info.allocationBase = region->first;
    info.allocationProtect = r.allocationProtect;
    info.regionSize = static_cast<std::uint32_t>((last - index + 1) * kPage);
    info.state = protection != 0 ? kMemCommit : kMemReserve;
    info.protect = protection;
    info.type = kMemPrivate;
    return info;
}

} // namespace sfcop::hle::kernel32rt
