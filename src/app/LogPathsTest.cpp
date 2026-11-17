#include "LogPaths.h"

#include <doctest/doctest.h>

namespace sfcop::app {

TEST_CASE("the home folder is written as ~ in a log") {
    const std::string home = "/Users/TestCaptain";
    CHECK(collapseHome("/Users/TestCaptain/Library/Application Support/SFCOP", home) ==
          "~/Library/Application Support/SFCOP");
    CHECK(collapseHome("/Users/TestCaptain", home) == "~");
    CHECK(collapseHome("/Users/TestCaptain/", home) == "~/");
    CHECK(collapseHome("/Users/TestCaptain/game", "/Users/TestCaptain/") == "~/game");
}

TEST_CASE("a path outside the home folder is left as it is") {
    const std::string home = "/Users/TestCaptain";
    CHECK(collapseHome("/Volumes/Games/Starfleet", home) == "/Volumes/Games/Starfleet");
    CHECK(collapseHome("/Users/TestCaptainJr/game", home) == "/Users/TestCaptainJr/game");
    CHECK(collapseHome("/Users", home) == "/Users");
    CHECK(collapseHome("", home) == "");
}

TEST_CASE("no usable home folder changes nothing") {
    CHECK(collapseHome("/Users/TestCaptain/game", "") == "/Users/TestCaptain/game");
    CHECK(collapseHome("/Users/TestCaptain/game", "/") == "/Users/TestCaptain/game");
}

TEST_CASE("a Windows home folder is collapsed on either separator") {
    CHECK(collapseHome("C:\\Users\\TestCaptain\\game", "C:\\Users\\TestCaptain") == "~\\game");
    CHECK(collapseHome("C:\\Users\\TestCaptain2\\game", "C:\\Users\\TestCaptain") ==
          "C:\\Users\\TestCaptain2\\game");
}

} // namespace sfcop::app
