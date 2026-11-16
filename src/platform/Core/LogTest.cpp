#include "platform/Core/Log.h"

#include <doctest/doctest.h>

#include <cstdio>
#include <string>

namespace sfcop::platform {
namespace {

std::string contents(FILE *file) {
    std::rewind(file);
    std::string text;
    char buffer[256];
    while (std::size_t n = std::fread(buffer, 1, sizeof buffer, file)) {
        text.append(buffer, n);
    }
    return text;
}

} // namespace

TEST_CASE("an important line lands in both files, a detail line only in the full one") {
    FILE *full = std::tmpfile();
    FILE *important = std::tmpfile();
    REQUIRE(full != nullptr);
    REQUIRE(important != nullptr);
    RouteLog(SFCOP_LOG_DETAIL, "[guest] App could not find 'X' to do connection\n", full, important);
    RouteLog(SFCOP_LOG_IMPORTANT, "[scripts] loaded m01 at 0x10000000\n", full, important);
    RouteLog(SFCOP_LOG_DETAIL, "[override] 0x00401000 f: 3 calls\n", full, important);
    CHECK(contents(full) == "[guest] App could not find 'X' to do connection\n"
                            "[scripts] loaded m01 at 0x10000000\n"
                            "[override] 0x00401000 f: 3 calls\n");
    CHECK(contents(important) == "[scripts] loaded m01 at 0x10000000\n");
    std::fclose(full);
    std::fclose(important);
}

TEST_CASE("with no important file open an important line goes to the full log only") {
    FILE *full = std::tmpfile();
    REQUIRE(full != nullptr);
    RouteLog(SFCOP_LOG_IMPORTANT, "fault\n", full, nullptr);
    CHECK(contents(full) == "fault\n");
    std::fclose(full);
}

} // namespace sfcop::platform
