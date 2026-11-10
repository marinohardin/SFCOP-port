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

TEST_CASE("a Linux or Windows package's hash list beside the executable is used") {
    MemoryFileSystem files;
    files.files["/opt/SFCOP-0.1.0-alpha-linux-x64/retail_hashes.sha256"] = "invented\n";
    files.files["C:\\Games\\SFCOP\\retail_hashes.sha256"] = "invented\n";
    CHECK(hashListFor("/opt/SFCOP-0.1.0-alpha-linux-x64/sfcop", kBuiltIn, files) ==
          "/opt/SFCOP-0.1.0-alpha-linux-x64/retail_hashes.sha256");
    CHECK(hashListFor("C:\\Games\\SFCOP\\sfcop.exe", kBuiltIn, files) ==
          "C:\\Games\\SFCOP\\retail_hashes.sha256");
    CHECK(hashListFor("/opt/elsewhere/sfcop", kBuiltIn, files) == kBuiltIn);
}

TEST_CASE("inside a bundle, the Resources copy wins over one beside the executable") {
    MemoryFileSystem files;
    files.files["/dist/SFCOP.app/Contents/Resources/retail_hashes.sha256"] = "invented\n";
    files.files["/dist/SFCOP.app/Contents/MacOS/retail_hashes.sha256"] = "invented\n";
    CHECK(hashListFor("/dist/SFCOP.app/Contents/MacOS/sfcop", kBuiltIn, files) ==
          "/dist/SFCOP.app/Contents/Resources/retail_hashes.sha256");
}

TEST_CASE("a package build: the list beside the client is used, and no install is a candidate") {
    MemoryFileSystem files;
    files.files["/opt/SFCOP-0.1.0-alpha-linux-x64/bin/retail_hashes.sha256"] = "invented\n";
    CHECK(hashListFor("/opt/SFCOP-0.1.0-alpha-linux-x64/bin/sfcop", "", files) ==
          "/opt/SFCOP-0.1.0-alpha-linux-x64/bin/retail_hashes.sha256");
    files.files["/dist/SFCOP.app/Contents/Resources/retail_hashes.sha256"] = "invented\n";
    CHECK(hashListFor("/dist/SFCOP.app/Contents/MacOS/sfcop", "", files) ==
          "/dist/SFCOP.app/Contents/Resources/retail_hashes.sha256");
    CHECK(hashListFor("/opt/elsewhere/sfcop", "", files) == "");
    CHECK(builtInInstallDirs("").empty());
    const std::vector<std::string> one = builtInInstallDirs("/games/Test Install");
    REQUIRE(one.size() == 1);
    CHECK(one[0] == "/games/Test Install");
}
