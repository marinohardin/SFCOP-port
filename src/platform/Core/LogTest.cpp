#include "platform/Core/Log.h"

#include <doctest/doctest.h>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

#if !defined(_WIN32)
#include <unistd.h>
#endif

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

std::string format(std::size_t size, const char *formatText, ...) {
    std::string out(size, '\x7F');
    std::va_list args;
    va_start(args, formatText);
    FormatLogLine(out.data(), size, formatText, args);
    va_end(args);
    return std::string(out.c_str());
}

std::string readFile(const char *path) {
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        return "";
    }
    std::string text = contents(file);
    std::fclose(file);
    return text;
}

} // namespace

TEST_CASE("a log line is formatted whole, and one too long is cut and still ends its line") {
    CHECK(format(64, "[scripts] refused %s: %d\n", "Met_17Patrol", 3) == "[scripts] refused Met_17Patrol: 3\n");
    CHECK(format(12, "%s\n", "abcdefghijklmnop") == "abcdefg...\n");
}

#if !defined(_WIN32) // mkstemp and setenv; the routing it uses is tested on every platform above
TEST_CASE("sfcop_log_init opens the file SFCOP_IMPORTANT_LOG names, and sfcop_logf writes to it") {
    const char *tmp = std::getenv("TMPDIR");
    std::string path = std::string(tmp != nullptr && *tmp != '\0' ? tmp : "/tmp") + "/sfcop-logtest-XXXXXX";
    const int fd = mkstemp(path.data());
    REQUIRE(fd >= 0);
    close(fd);
    REQUIRE(setenv("SFCOP_IMPORTANT_LOG", path.c_str(), 1) == 0);
    sfcop_log_init();
    sfcop_logf(SFCOP_LOG_DETAIL, "[guest] detail %d\n", 1);
    sfcop_logf(SFCOP_LOG_IMPORTANT, "=== SFCOP exit, code %d\n", 0);
    CHECK(readFile(path.c_str()) == "=== SFCOP exit, code 0\n");
    unsetenv("SFCOP_IMPORTANT_LOG");
    std::remove(path.c_str());
}
#endif

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
