#include "hle/kernel32rt/HostMemory.h"

#include <doctest/doctest.h>

#include <vector>

#if !defined(_WIN32)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace sfcop::hle::kernel32rt {
namespace {

TEST_CASE("memory the program owns is accessible for reading and writing") {
    std::vector<char> block(10000);
    CHECK(hostRangeAccessible(block.data(), block.size(), false));
    CHECK(hostRangeAccessible(block.data(), block.size(), true));
}

TEST_CASE("the first page of the address space is not accessible") {
    CHECK_FALSE(hostRangeAccessible(reinterpret_cast<const void *>(16), 4, false));
}

TEST_CASE("an empty range is accessible wherever it is") {
    CHECK(hostRangeAccessible(nullptr, 0, true));
}

#if !defined(_WIN32)
TEST_CASE("a range that runs into a page with no access is not accessible") {
    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    void *two = mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    REQUIRE(two != MAP_FAILED);
    auto *bytes = static_cast<char *>(two);
    REQUIRE(mprotect(bytes + page, page, PROT_NONE) == 0);

    CHECK(hostRangeAccessible(bytes, page, false)); // the first page alone
    CHECK_FALSE(hostRangeAccessible(bytes + page, 16, false)); // the second alone
    CHECK_FALSE(hostRangeAccessible(bytes + page - 8, 16, false)); // 8 bytes in, 8 beyond
    CHECK_FALSE(hostRangeAccessible(bytes, 2 * page, false));
    munmap(two, 2 * page);
}
#endif

} // namespace
} // namespace sfcop::hle::kernel32rt
