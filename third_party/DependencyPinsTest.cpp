// The libraries the build links are the releases third_party/CMakeLists.txt pins.

#include <SDL3/SDL.h>
#include <doctest/doctest.h>
#include <miniaudio.h>

#include <string>

TEST_CASE("the linked SDL3 is the pinned release 3.4.18") {
    CHECK(SDL_GetVersion() == SDL_VERSIONNUM(3, 4, 18));
}

TEST_CASE("the linked miniaudio is the pinned release 0.11.25") {
    CHECK(std::string(ma_version_string()) == "0.11.25");
}

TEST_CASE("the doctest headers are the pinned release 2.5.3") {
    CHECK(std::string(DOCTEST_VERSION_STR) == "2.5.3");
}
