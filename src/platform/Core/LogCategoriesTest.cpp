#include "platform/Core/LogCategories.h"

#include <doctest/doctest.h>

#include <cstdlib>

namespace sfcop::platform {

TEST_CASE("a log category is on only when the list names it or all") {
    CHECK_FALSE(LogCategoryInList("", "guest-debug"));
    CHECK_FALSE(LogCategoryInList("override", "guest-debug"));
    CHECK(LogCategoryInList("guest-debug", "guest-debug"));
    CHECK(LogCategoryInList("override, guest-debug", "guest-debug"));
    CHECK(LogCategoryInList(" guest-debug ,override", "override"));
    CHECK(LogCategoryInList("all", "override"));
    CHECK_FALSE(LogCategoryInList("guest-debugger", "guest-debug"));
    CHECK_FALSE(LogCategoryInList(",,", "override"));
    CHECK_FALSE(LogCategoryInList("nonsense", "override"));
}

#ifndef _WIN32
TEST_CASE("sfcop_log_enabled reads SFCOP_LOG") {
    const char *saved = std::getenv("SFCOP_LOG");
    const std::string keep = saved != nullptr ? saved : "";
    unsetenv("SFCOP_LOG");
    CHECK_FALSE(sfcop_log_enabled("guest-debug"));
    setenv("SFCOP_LOG", "guest-debug", 1);
    CHECK(sfcop_log_enabled("guest-debug"));
    CHECK_FALSE(sfcop_log_enabled("override"));
    if (saved != nullptr) {
        setenv("SFCOP_LOG", keep.c_str(), 1);
    } else {
        unsetenv("SFCOP_LOG");
    }
}

TEST_CASE("AddLogCategories adds to what SFCOP_LOG already names") {
    unsetenv("SFCOP_LOG");
    AddLogCategories("override");
    AddLogCategories("guest-debug");
    CHECK(sfcop_log_enabled("override"));
    CHECK(sfcop_log_enabled("guest-debug"));
    unsetenv("SFCOP_LOG");
}
#endif

} // namespace sfcop::platform
