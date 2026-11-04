// VirtualMemory: VirtualAlloc, VirtualFree and VirtualQuery over the guest's 32-bit address space.
// The game's C runtime and its allocator reserve regions and commit pages in them.
//
// It keeps the regions and the state of each 4 KB page (reserved or committed, and its
// protection); the address space itself comes from a Backend, which in the build is the guest
// block's reservations (pcrecomp's hle_vm_*). Protection is recorded and reported, not enforced:
// every page the backend maps is readable and writable. A page outside every region is described
// as the backend sees it (the image and the heap read as committed; the rest as free).
//
// Threading: any thread; behind a lock.
#pragma once

#include "hle/kernel32rt/Kernel32rt.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

namespace sfcop::hle::kernel32rt {

constexpr std::uint32_t kMemCommit = 0x1000u;
constexpr std::uint32_t kMemReserve = 0x2000u;
constexpr std::uint32_t kMemDecommit = 0x4000u;
constexpr std::uint32_t kMemRelease = 0x8000u;
constexpr std::uint32_t kMemFree = 0x10000u;
constexpr std::uint32_t kMemPrivate = 0x20000u;
constexpr std::uint32_t kPageNoAccess = 0x01u;
constexpr std::uint32_t kPageReadWrite = 0x04u;
constexpr std::uint32_t kErrorInvalidAddress = 487;

// MEMORY_BASIC_INFORMATION's seven fields, in its order.
struct MemoryInfo {
    std::uint32_t baseAddress = 0;
    std::uint32_t allocationBase = 0;
    std::uint32_t allocationProtect = 0;
    std::uint32_t regionSize = 0;
    std::uint32_t state = 0;
    std::uint32_t protect = 0;
    std::uint32_t type = 0;
};

class VirtualMemory {
public:
    struct Backend {
        // Reserve `size` bytes at `address` (0: anywhere); the base, or 0. New pages read zero.
        std::function<std::uint32_t(std::uint32_t address, std::uint32_t size)> reserve;
        std::function<void(std::uint32_t address, std::uint32_t size)> release;
        // Drop the pages' contents: they read as zero again.
        std::function<void(std::uint32_t address, std::uint32_t size)> zero;
        // Whether a page outside every region is mapped (the image, the heap).
        std::function<bool(std::uint32_t page)> mapped;
    };

    explicit VirtualMemory(Backend backend);

    // VirtualAlloc(address, size, type, protect): the region's or the committed range's base, or
    // 0 with ERROR_INVALID_PARAMETER (a bad type or size), ERROR_INVALID_ADDRESS (a commit
    // outside every reservation, or a reservation where one is) or ERROR_NOT_ENOUGH_MEMORY.
    Answer Allocate(std::uint32_t address, std::uint32_t size, std::uint32_t type,
                    std::uint32_t protect);
    // VirtualFree(address, size, type): 1, or 0 with ERROR_INVALID_PARAMETER (a release that is
    // not of a whole region with size 0, an address in no region, a bad type).
    Answer Free(std::uint32_t address, std::uint32_t size, std::uint32_t type);
    // VirtualQuery: the run of pages from `address`'s page that share a state and protection.
    MemoryInfo Query(std::uint32_t address);

private:
    struct Region {
        std::uint32_t size = 0;
        std::uint32_t allocationProtect = 0;
        std::vector<std::uint32_t> pages; // 0: reserved; otherwise committed with this protection
    };

    // The region holding `address`: its base, or end() of the map.
    std::map<std::uint32_t, Region>::iterator regionOf(std::uint32_t address);
    Answer reserve(std::uint32_t address, std::uint32_t size, std::uint32_t type,
                   std::uint32_t protect);

    Backend backend_;
    std::mutex lock_;
    std::map<std::uint32_t, Region> regions_; // by base
};

} // namespace sfcop::hle::kernel32rt
