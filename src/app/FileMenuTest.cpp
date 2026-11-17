#include "app/FileMenu.h"

#include <doctest/doctest.h>

namespace sfcop::app {

TEST_CASE("the game files item shows the resolved data folder") {
    const auto entries =
        fileMenuEntries("/Volumes/Test Drive/TestCaptain/data",
                        "/Volumes/Test Drive/TestCaptain/data/logs");
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].title == "Show Game Files in Finder");
    CHECK(entries[0].path == "/Volumes/Test Drive/TestCaptain/data");
    CHECK(entries[1].title == "Show Logs in Finder");
    CHECK(entries[1].path == "/Volumes/Test Drive/TestCaptain/data/logs");
}

TEST_CASE("an entry without a folder is left out") {
    CHECK(fileMenuEntries("", "").empty());
    const auto entries = fileMenuEntries("/data", "");
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].title == "Show Game Files in Finder");
}

TEST_CASE("choosing an entry reveals its path through the seam") {
    const auto entries = fileMenuEntries("/data", "/logs");
    std::vector<std::string> shown;
    const RevealFn reveal = [&shown](const std::string &path) {
        shown.push_back(path);
        return true;
    };
    CHECK(activateFileMenuEntry(entries, 0, reveal));
    CHECK(activateFileMenuEntry(entries, 1, reveal));
    REQUIRE(shown.size() == 2);
    CHECK(shown[0] == "/data");
    CHECK(shown[1] == "/logs");
}

TEST_CASE("a bad index or a failing reveal shows nothing and reports it") {
    const auto entries = fileMenuEntries("/data", "/logs");
    int calls = 0;
    CHECK_FALSE(activateFileMenuEntry(entries, 2, [&calls](const std::string &) {
        ++calls;
        return true;
    }));
    CHECK(calls == 0);
    CHECK_FALSE(activateFileMenuEntry(entries, 0, [](const std::string &) { return false; }));
    CHECK_FALSE(activateFileMenuEntry(entries, 0, RevealFn{}));
}

} // namespace sfcop::app
