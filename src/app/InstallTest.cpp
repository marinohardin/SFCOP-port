#include "app/Install.h"

#include <cstdlib>

#include <doctest/doctest.h>

#include "app/FilesMock.h"

using namespace sfcop::app;
using sfcop::app::testing::fixturesDir;
using sfcop::app::testing::MemoryFileSystem;
using sfcop::app::testing::OneByteAltered;

namespace {

const char kExeHash[] = "a8efff83a244d64032d614355c235f36cfc10808403a6e1b00f6c18265c36cdc";
const char kEmptyHash[] = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

std::string fixtureInstall() { return fixturesDir() + "/install"; }

std::vector<HashEntry> fixtureEntries() {
    const std::optional<std::string> text =
        hostFileSystem().readText(fixturesDir() + "/install.sha256");
    REQUIRE(text.has_value());
    const HashList list = parseHashList(*text);
    REQUIRE(list.ok());
    return list.entries;
}

EnvLookup envOf(std::map<std::string, std::string> values) {
    return [values](const std::string &name) -> std::optional<std::string> {
        const auto found = values.find(name);
        if (found == values.end()) {
            return std::nullopt;
        }
        return found->second;
    };
}

} // namespace

TEST_CASE("a hash list in sha256sum's format is read in order, comments and blanks skipped") {
    const HashList list = parseHashList(
        "# a comment\n"
        "\n"
        "A8EFFF83A244D64032D614355C235F36CFC10808403A6E1B00F6C18265C36CDC  StarFleetOP.exe\r\n"
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 *Sounds/empty file.txt");
    REQUIRE(list.ok());
    REQUIRE(list.entries.size() == 2);
    CHECK(list.entries[0].sha256 == kExeHash);
    CHECK(list.entries[0].path == "StarFleetOP.exe");
    CHECK(list.entries[0].line == 3);
    CHECK(list.entries[1].sha256 == kEmptyHash);
    CHECK(list.entries[1].path == "Sounds/empty file.txt");
    CHECK(list.entries[1].line == 4);
}

TEST_CASE("a hash list line in the wrong format is refused with its line number") {
    const std::string good = std::string(kExeHash) + "  StarFleetOP.exe\n";
    CHECK(parseHashList(good + "a8efff83  Short.exe\n").error ==
          "line 2: expected 64 hex digits of SHA-256 at the start");
    CHECK(parseHashList(good + "\n" + std::string(kEmptyHash) + "0  Bad.txt\n").error ==
          "line 3: expected 64 hex digits of SHA-256 at the start");
    CHECK(parseHashList(std::string(kEmptyHash) + " one-space.txt\n").error ==
          "line 1: expected two spaces, or a space and '*', then a path");
    CHECK(parseHashList(std::string(kEmptyHash) + "  \n").error ==
          "line 1: expected two spaces, or a space and '*', then a path");
    CHECK(parseHashList(good + std::string(kEmptyHash) + "  ../outside.txt\n").error ==
          "line 2: the path '../outside.txt' must be relative, with '/' between parts, and stay "
          "inside the root");
    CHECK(parseHashList(std::string(kEmptyHash) + "  /etc/hosts\n").error ==
          "line 1: the path '/etc/hosts' must be relative, with '/' between parts, and stay inside "
          "the root");
    CHECK(parseHashList(std::string(kEmptyHash) + "  Sounds\\hail.wav\n").error ==
          "line 1: the path 'Sounds\\hail.wav' must be relative, with '/' between parts, and stay "
          "inside the root");
    CHECK(parseHashList(std::string(kEmptyHash) + "  Sounds//hail.wav\n").error ==
          "line 1: the path 'Sounds//hail.wav' must be relative, with '/' between parts, and stay "
          "inside the root");
    CHECK(parseHashList(good + good).error ==
          "line 2: 'StarFleetOP.exe' is listed twice (first on line 1)");
}

TEST_CASE("the install's entries are found inside a list that names other products too") {
    const std::string kit = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const HashList list = parseHashList(
        std::string(kExeHash) + "  games/Orion/StarFleetOP.exe\n" + kit +
        "  Server Kit/Assets/StarFleetOP.exe\n" + kit + "  Server Kit/Platform.exe\n" + kEmptyHash +
        "  games/Orion/Sounds/empty.txt\n" + kEmptyHash + "  games/Orion/SDK/Lib/Test.lib\n" +
        kEmptyHash + "  games/Orionette/Other.txt\n");
    REQUIRE(list.ok());
    const HashList inside = installEntries(list);
    REQUIRE(inside.ok());
    REQUIRE(inside.entries.size() == 3);
    CHECK(inside.entries[0].path == "StarFleetOP.exe");
    CHECK(inside.entries[0].line == 1);
    CHECK(inside.entries[1].path == "Sounds/empty.txt");
    CHECK(inside.entries[2].path == "SDK/Lib/Test.lib");
    CHECK(inside.entries[2].line == 5);
}

TEST_CASE("a list already relative to the install is kept whole") {
    const HashList inside = installEntries(parseHashList(
        std::string(kEmptyHash) + "  Sounds/empty.txt\n" + kExeHash + "  StarFleetOP.exe\n"));
    REQUIRE(inside.ok());
    REQUIRE(inside.entries.size() == 2);
    CHECK(inside.entries[0].path == "Sounds/empty.txt");
    CHECK(inside.entries[1].path == "StarFleetOP.exe");
}

TEST_CASE("a list without the game executable has no install in it") {
    CHECK(installEntries(parseHashList(std::string(kEmptyHash) + "  Sounds/empty.txt\n")).error ==
          "the list names no StarFleetOP.exe");
    CHECK(installEntries(parseHashList("nonsense\n")).error ==
          "line 1: expected 64 hex digits of SHA-256 at the start");
}

TEST_CASE("the invented fixture install is accepted") {
    std::vector<VerifyProgress> seen;
    const VerifyResult result =
        verifyInstall(fixtureInstall(), fixtureEntries(), hostFileSystem(),
                      [&seen](const VerifyProgress &p) { seen.push_back(p); });
    CHECK(result.accepted());
    CHECK(result.exitCode() == 0);
    // One piece for each non-empty file; the empty one is read in none.
    REQUIRE(seen.size() == 4);
    CHECK(seen[0].path == "StarFleetOP.exe");
    CHECK(seen[0].fileIndex == 0);
    CHECK(seen[0].fileCount == 5);
    CHECK(seen[0].bytesDone == 81);
    CHECK(seen[0].bytesTotal == 213);
    CHECK(seen[3].path == "Sounds/hail.wav");
    CHECK(seen[3].bytesDone == 213);
}

TEST_CASE("one altered byte in the fixture install is refused, naming the file") {
    const std::string target = fixtureInstall() + "/Assets/Missions/Skirmish/Test Patrol.ini";
    const OneByteAltered altered(hostFileSystem(), target, 30);
    const VerifyResult result = verifyInstall(fixtureInstall(), fixtureEntries(), altered);
    CHECK(result.status == VerifyStatus::kMismatch);
    CHECK(result.path == "Assets/Missions/Skirmish/Test Patrol.ini");
    CHECK(result.exitCode() == 3);
    CHECK(result.message() ==
          "Assets/Missions/Skirmish/Test Patrol.ini in the install at " + fixtureInstall() +
              " differs from the 2.5.5.2 release. Reinstall Starfleet Command: Orion Pirates from "
              "its disc and apply the official 2.5.5.2 patch, then start sfcop again.");
}

TEST_CASE("a file allowed to differ is accepted and listed; others are still refused") {
    const std::string target = fixtureInstall() + "/Assets/Missions/Skirmish/Test Patrol.ini";
    const OneByteAltered altered(hostFileSystem(), target, 30);
    // Allowed by another spelling: the import matched names ignoring case.
    const VerifyResult allowed =
        verifyInstall(fixtureInstall(), fixtureEntries(), altered, {},
                      {"assets/missions/skirmish/test patrol.INI", "Sounds/hail.wav"});
    CHECK(allowed.accepted());
    CHECK(allowed.differing == std::vector<std::string>{"Assets/Missions/Skirmish/Test Patrol.ini"});

    const VerifyResult other =
        verifyInstall(fixtureInstall(), fixtureEntries(), altered, {}, {"Sounds/hail.wav"});
    CHECK(other.status == VerifyStatus::kMismatch);
    CHECK(other.path == "Assets/Missions/Skirmish/Test Patrol.ini");

    // Allowed to differ is not allowed to be missing.
    std::vector<HashEntry> entries = fixtureEntries();
    entries.push_back({kEmptyHash, "Sounds/not there.wav", 9});
    const VerifyResult missing =
        verifyInstall(fixtureInstall(), entries, hostFileSystem(), {}, {"Sounds/not there.wav"});
    CHECK(missing.status == VerifyStatus::kMissing);
    CHECK(missing.path == "Sounds/not there.wav");
}

TEST_CASE("verification stops at the first bad file in list order") {
    MemoryFileSystem files;
    files.files["/op/StarFleetOP.exe"] = "x";
    files.files["/op/b.txt"] = "";
    const HashList list = parseHashList(std::string(kEmptyHash) + "  a.txt\n" + kEmptyHash +
                                        "  StarFleetOP.exe\n" + kEmptyHash + "  b.txt\n");
    REQUIRE(list.ok());
    const VerifyResult result = verifyInstall("/op", list.entries, files);
    CHECK(result.status == VerifyStatus::kMissing);
    CHECK(result.path == "a.txt");
}

TEST_CASE("a missing file is refused, naming the file") {
    std::vector<HashEntry> entries = fixtureEntries();
    entries.push_back({kEmptyHash, "Sounds/not there.wav", 9});
    const VerifyResult result = verifyInstall(fixtureInstall(), entries, hostFileSystem());
    CHECK(result.status == VerifyStatus::kMissing);
    CHECK(result.path == "Sounds/not there.wav");
    CHECK(result.exitCode() == 3);
    CHECK(result.message() ==
          "Sounds/not there.wav is missing from the install at " + fixtureInstall() +
              ". Reinstall Starfleet Command: Orion Pirates from its disc and apply the official "
              "2.5.5.2 patch, then start sfcop again.");
}

TEST_CASE("a file that cannot be opened or read is refused, naming the file") {
    MemoryFileSystem files;
    files.files["/op/StarFleetOP.exe"] = "x";
    const std::vector<HashEntry> entries = {{kEmptyHash, "StarFleetOP.exe", 1}};

    files.unopenable.insert("/op/StarFleetOP.exe");
    VerifyResult result = verifyInstall("/op", entries, files);
    CHECK(result.status == VerifyStatus::kUnreadable);
    CHECK(result.path == "StarFleetOP.exe");

    files.unopenable.clear();
    files.failAfter["/op/StarFleetOP.exe"] = 0;
    result = verifyInstall("/op", entries, files);
    CHECK(result.status == VerifyStatus::kUnreadable);
    CHECK(result.exitCode() == 3);
}

TEST_CASE("a large file is hashed in pieces of at most 1 MiB") {
    MemoryFileSystem files;
    files.files["/op/StarFleetOP.exe"] = std::string(2621440, 'a'); // 2.5 MiB
    std::vector<std::uint64_t> done;
    const std::vector<HashEntry> entries = {
        {"b19fda75b6c96f0cd6a27a6c371352dbe76e5a5f4ac1a10fb1352df725eb04be", "StarFleetOP.exe", 1}};
    const VerifyResult result = verifyInstall(
        "/op", entries, files, [&done](const VerifyProgress &p) { done.push_back(p.bytesDone); });
    CHECK(result.accepted());
    CHECK(done == std::vector<std::uint64_t>{1048576, 2097152, 2621440});
}

TEST_CASE("--install is the only candidate when given") {
    MemoryFileSystem files;
    files.files["/remembered/StarFleetOP.exe"] = "x";
    files.files["/flag/StarFleetOP.exe"] = "x";
    LocateRequest request;
    request.flag = "/flag";
    request.remembered = "/remembered";
    LocateResult result = locateInstall(request, files);
    CHECK(result.dir == std::optional<std::string>("/flag"));
    CHECK(result.source == InstallSource::kFlag);

    request.flag = "/empty";
    result = locateInstall(request, files);
    CHECK_FALSE(result.dir.has_value());
    CHECK(result.error == "--install /empty holds no StarFleetOP.exe.");
}

TEST_CASE("the remembered install comes before the defaults, and a stale one is skipped") {
    MemoryFileSystem files;
    files.files["/remembered/StarFleetOP.exe"] = "x";
    files.files["/default2/StarFleetOP.exe"] = "x";
    LocateRequest request;
    request.remembered = "/remembered";
    request.defaults = {"/default1", "/default2"};
    LocateResult result = locateInstall(request, files);
    CHECK(result.dir == std::optional<std::string>("/remembered"));
    CHECK(result.source == InstallSource::kRemembered);

    request.remembered = "/moved";
    result = locateInstall(request, files);
    CHECK(result.dir == std::optional<std::string>("/default2"));
    CHECK(result.source == InstallSource::kDefault);
}

TEST_CASE("the player is asked once when no candidate holds an install") {
    MemoryFileSystem files;
    files.files["/answer/StarFleetOP.exe"] = "x";
    int asked = 0;
    LocateRequest request;
    request.defaults = {"/default1"};
    request.ask = [&asked]() -> std::optional<std::string> {
        ++asked;
        return std::string("/answer");
    };
    LocateResult result = locateInstall(request, files);
    CHECK(asked == 1);
    CHECK(result.dir == std::optional<std::string>("/answer"));
    CHECK(result.source == InstallSource::kAsked);

    request.ask = []() -> std::optional<std::string> { return std::string("/wrong"); };
    result = locateInstall(request, files);
    CHECK(result.error == "/wrong holds no StarFleetOP.exe.");

    request.ask = nullptr;
    result = locateInstall(request, files);
    CHECK(result.source == InstallSource::kNone);
    CHECK(result.error == "No install of Starfleet Command: Orion Pirates was found. Give its "
                          "folder, the one that holds StarFleetOP.exe, with --install DIR.");
}

TEST_CASE("the default install folders follow each platform") {
    CHECK(defaultInstallDirs(Platform::kWindows,
                             envOf({{"ProgramFiles(x86)", "C:\\Program Files (x86)"},
                                    {"ProgramFiles", "C:\\Program Files"}})) ==
          std::vector<std::string>{
              "C:\\Program Files (x86)\\Taldren\\Starfleet Command Orion Pirates",
              "C:\\Program Files\\Taldren\\Starfleet Command Orion Pirates"});
    CHECK(defaultInstallDirs(Platform::kWindows, envOf({{"ProgramFiles", "D:\\Games\\"}})) ==
          std::vector<std::string>{"D:\\Games\\Taldren\\Starfleet Command Orion Pirates"});
    CHECK(defaultInstallDirs(Platform::kMacos, envOf({{"HOME", "/Users/testcaptain"}})) ==
          std::vector<std::string>{
              "/Users/testcaptain/.wine/drive_c/Program Files (x86)/Taldren/Starfleet Command "
              "Orion Pirates",
              "/Users/testcaptain/.wine/drive_c/Program Files/Taldren/Starfleet Command Orion "
              "Pirates"});
    CHECK(defaultInstallDirs(Platform::kLinux, envOf({{"HOME", "/home/testcaptain"},
                                                      {"WINEPREFIX", "/home/testcaptain/op"}})) ==
          std::vector<std::string>{
              "/home/testcaptain/op/drive_c/Program Files (x86)/Taldren/Starfleet Command Orion "
              "Pirates",
              "/home/testcaptain/op/drive_c/Program Files/Taldren/Starfleet Command Orion "
              "Pirates"});
    CHECK(defaultInstallDirs(Platform::kLinux, envOf({})).empty());
}

namespace {

// SFCOP_INSTALL_DIR names the retail install where there is one (the runner, the owner's Mac).
std::optional<std::string> retailInstall() {
    const char *dir = std::getenv("SFCOP_INSTALL_DIR");
    if (dir == nullptr || !holdsInstall(dir, hostFileSystem())) {
        return std::nullopt;
    }
    return std::string(dir);
}

// The decompilation's hash list (SFCOP_DECOMP_DIR, cmake/SfcopDecomp.cmake).
HashList repositoryInstallEntries() {
    const std::optional<std::string> text = hostFileSystem().readText(
        std::string(SFCOP_TEST_DECOMP_DIR) + "/analysis/results/retail_hashes.sha256");
    REQUIRE(text.has_value());
    return installEntries(parseHashList(*text));
}

} // namespace

TEST_CASE("the repository's hash list parses and names the game executable first") {
    const HashList inside = repositoryInstallEntries();
    REQUIRE(inside.ok());
    REQUIRE_FALSE(inside.entries.empty());
    CHECK(inside.entries[0].path == "StarFleetOP.exe");
}

TEST_CASE("the retail install matches the repository's hash list" *
          doctest::skip(!retailInstall().has_value())) {
    const VerifyResult result =
        verifyInstall(*retailInstall(), repositoryInstallEntries().entries, hostFileSystem());
    CHECK_MESSAGE(result.accepted(), result.message());
}

TEST_CASE("a run verifies the install without the boot stub and the SDK's libraries") {
    const HashList list = parseHashList(
        "1111111111111111111111111111111111111111111111111111111111111111  StarFleetOP.exe\n"
        "2222222222222222222222222222222222222222222222222222222222222222  SDK/Libs/Shared.LIB\n"
        "3333333333333333333333333333333333333333333333333333333333333333  SFCOP.exe\n"
        "4444444444444444444444444444444444444444444444444444444444444444  Assets/a.mod\n"
        "5555555555555555555555555555555555555555555555555555555555555555  Assets/SFCOP.exe\n");
    REQUIRE(list.ok());
    const std::vector<HashEntry> kept = runtimeEntries(list.entries);
    REQUIRE(kept.size() == 3);
    CHECK(kept[0].path == "StarFleetOP.exe");
    CHECK(kept[1].path == "Assets/a.mod");
    CHECK(kept[2].path == "Assets/SFCOP.exe");
}
