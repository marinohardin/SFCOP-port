#include "app/Bundle.h"

#include <doctest/doctest.h>

#include "app/FilesMock.h"

using namespace sfcop::app;
using sfcop::app::testing::MemoryFileSystem;

namespace {

constexpr const char kBuiltIn[] = "/src/SFCOP-decomp/analysis/results/retail_hashes.sha256";

} // namespace

TEST_CASE("an executable in <bundle>/Contents/MacOS has its resources in Contents/Resources") {
    CHECK(bundleResourcesDir("/Users/testcaptain/dist/SFCOP.app/Contents/MacOS/sfcop") ==
          "/Users/testcaptain/dist/SFCOP.app/Contents/Resources");
    CHECK(bundleResourcesDir("/Applications/SFCOP.app/Contents/MacOS/sfcop") ==
          "/Applications/SFCOP.app/Contents/Resources");
    CHECK(bundleResourcesDir("/work/build/sfcop") == "");
    CHECK(bundleResourcesDir("/work/MacOS/sfcop") == ""); // no Contents above MacOS
    CHECK(bundleResourcesDir("/Contents/MacOS/") == "");  // no executable
    CHECK(bundleResourcesDir("sfcop") == "");
    CHECK(bundleResourcesDir("") == "");
}

TEST_CASE("the bundle's hash list is used when the client runs from a bundle that carries one") {
    MemoryFileSystem files;
    files.files["/dist/SFCOP.app/Contents/Resources/retail_hashes.sha256"] = "invented\n";
    CHECK(hashListFor("/dist/SFCOP.app/Contents/MacOS/sfcop", kBuiltIn, files) ==
          "/dist/SFCOP.app/Contents/Resources/retail_hashes.sha256");
}

TEST_CASE("a build run in place, or a bundle without the list, keeps the compiled-in hash list") {
    MemoryFileSystem files;
    files.files["/dist/SFCOP.app/Contents/Resources/retail_hashes.sha256"] = "invented\n";
    CHECK(hashListFor("/work/L1a/build/sfcop", kBuiltIn, files) == kBuiltIn);
    CHECK(hashListFor("", kBuiltIn, files) == kBuiltIn); // the executable could not be found
    MemoryFileSystem empty;
    CHECK(hashListFor("/dist/SFCOP.app/Contents/MacOS/sfcop", kBuiltIn, empty) == kBuiltIn);
}
