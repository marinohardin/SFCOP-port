#include "hle/kernel32rt/VirtualMemory.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace sfcop::hle::kernel32rt {
namespace {

// A backend that hands out the address it is asked for, or 0x01000000 for "anywhere", and
// records what it was asked.
struct Recorder {
    std::vector<std::string> calls;
    std::uint32_t next = 0x01000000u;

    VirtualMemory::Backend Backend() {
        VirtualMemory::Backend backend;
        backend.reserve = [this](std::uint32_t at, std::uint32_t size) {
            calls.push_back("reserve " + std::to_string(at) + " " + std::to_string(size));
            return at != 0 ? at : next;
        };
        backend.release = [this](std::uint32_t at, std::uint32_t size) {
            calls.push_back("release " + std::to_string(at) + " " + std::to_string(size));
        };
        backend.zero = [this](std::uint32_t at, std::uint32_t size) {
            calls.push_back("zero " + std::to_string(at) + " " + std::to_string(size));
        };
        backend.mapped = [](std::uint32_t page) { return page >= 0x00400000u && page < 0x00800000u; };
        return backend;
    }
};

TEST_CASE("a reservation is reported reserved, and a commit inside it committed") {
    Recorder backend;
    VirtualMemory memory(backend.Backend());
    const Answer reserved = memory.Allocate(0, 0x100000, kMemReserve, kPageReadWrite);
    CHECK(reserved.value == 0x01000000u);
    CHECK(backend.calls == std::vector<std::string>{"reserve 0 1048576"});
    const Answer committed = memory.Allocate(0x01002345u, 0x2000, kMemCommit, kPageReadWrite);
    CHECK(committed.value == 0x01002000u);

    const MemoryInfo before = memory.Query(0x01000000u);
    CHECK(before.state == kMemReserve);
    CHECK(before.regionSize == 0x2000u);
    CHECK(before.allocationBase == 0x01000000u);
    const MemoryInfo inside = memory.Query(0x01003000u);
    CHECK(inside.baseAddress == 0x01003000u);
    CHECK(inside.state == kMemCommit);
    CHECK(inside.protect == kPageReadWrite);
    CHECK(inside.regionSize == 0x2000u); // 0x01003000 and the page the commit's end reached
    CHECK(inside.type == kMemPrivate);
}

TEST_CASE("a commit at no address reserves and commits") {
    Recorder backend;
    VirtualMemory memory(backend.Backend());
    CHECK(memory.Allocate(0, 0x1800, kMemCommit, kPageReadWrite).value == 0x01000000u);
    CHECK(backend.calls == std::vector<std::string>{"reserve 0 8192"});
    CHECK(memory.Query(0x01001000u).state == kMemCommit);
}

TEST_CASE("decommitting drops the pages and leaves them reserved") {
    Recorder backend;
    VirtualMemory memory(backend.Backend());
    memory.Allocate(0, 0x4000, kMemReserve | kMemCommit, kPageReadWrite);
    CHECK(memory.Free(0x01001000u, 0x1000, kMemDecommit).value == 1);
    CHECK(backend.calls.back() == "zero 16781312 4096");
    CHECK(memory.Query(0x01001000u).state == kMemReserve);
    CHECK(memory.Query(0x01002000u).state == kMemCommit);
}

TEST_CASE("a release needs the region's base and size 0") {
    Recorder backend;
    VirtualMemory memory(backend.Backend());
    memory.Allocate(0, 0x4000, kMemReserve, kPageReadWrite);
    CHECK(memory.Free(0x01001000u, 0, kMemRelease).lastError == kErrorInvalidParameter);
    CHECK(memory.Free(0x01000000u, 0x4000, kMemRelease).lastError == kErrorInvalidParameter);
    CHECK(memory.Free(0x01000000u, 0, kMemRelease).value == 1);
    CHECK(backend.calls.back() == "release 16777216 16384");
    CHECK(memory.Free(0x01000000u, 0, kMemRelease).lastError == kErrorInvalidParameter);
}

TEST_CASE("a commit outside every reservation fails with ERROR_INVALID_ADDRESS") {
    Recorder backend;
    VirtualMemory memory(backend.Backend());
    const Answer answer = memory.Allocate(0x02000000u, 0x1000, kMemCommit, kPageReadWrite);
    CHECK(answer.value == 0);
    CHECK(answer.lastError == kErrorInvalidAddress);
}

TEST_CASE("a reservation where one already is fails, and its address is rounded to 64 KB") {
    Recorder backend;
    VirtualMemory memory(backend.Backend());
    CHECK(memory.Allocate(0x03001234u, 0x1000, kMemReserve, kPageReadWrite).value == 0x03000000u);
    CHECK(backend.calls == std::vector<std::string>{"reserve 50331648 12288"});
    CHECK(memory.Allocate(0x03000000u, 0x1000, kMemReserve, kPageReadWrite).lastError ==
          kErrorInvalidAddress);
}

TEST_CASE("a reservation below 64 KB is refused rather than placed anywhere") {
    Recorder backend;
    VirtualMemory memory(backend.Backend());
    const Answer answer = memory.Allocate(0x00004000u, 0x1000, kMemReserve, kPageReadWrite);
    CHECK(answer.value == 0);
    CHECK(answer.lastError == kErrorInvalidAddress);
    CHECK(backend.calls.empty());
    // No address at all still means anywhere.
    CHECK(memory.Allocate(0, 0x1000, kMemReserve, kPageReadWrite).value == 0x01000000u);
}

TEST_CASE("VirtualAlloc refuses a size of 0 and an unknown type") {
    Recorder backend;
    VirtualMemory memory(backend.Backend());
    CHECK(memory.Allocate(0, 0, kMemReserve, kPageReadWrite).lastError == kErrorInvalidParameter);
    CHECK(memory.Allocate(0, 0x1000, kMemDecommit, kPageReadWrite).lastError ==
          kErrorInvalidParameter);
    CHECK(backend.calls.empty());
}

TEST_CASE("a page outside every region reads as the backend maps it") {
    Recorder backend;
    VirtualMemory memory(backend.Backend());
    const MemoryInfo image = memory.Query(0x00401234u);
    CHECK(image.baseAddress == 0x00401000u);
    CHECK(image.state == kMemCommit);
    CHECK(image.allocationBase == 0x00400000u);
    const MemoryInfo nothing = memory.Query(0x7000F000u);
    CHECK(nothing.state == kMemFree);
    CHECK(nothing.protect == kPageNoAccess);
}

} // namespace
} // namespace sfcop::hle::kernel32rt
