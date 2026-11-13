// What the launcher's window is set up from: suggestions, the game in use, the folder picker and
// the manifest.
#include "app/launcher/LauncherSetup.h"

#include <doctest/doctest.h>

#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include "app/DataDir.h"
#include "app/FilesMock.h"
#include "app/Import.h"

namespace sfcop::app::launcher {

namespace {

const std::string kData = "/home/TestCaptain/.local/share/SFCOP";
const std::string kGame = kData + "/game";
const std::string kExe = "StarFleetOP.exe";

EnvLookup envOf(std::optional<std::string> bus) {
    return [bus](const std::string &name) -> std::optional<std::string> {
        if (name == "DBUS_SESSION_BUS_ADDRESS") {
            return bus;
        }
        return std::nullopt;
    };
}

std::string fixtureText(const std::string &name) {
    std::ifstream file(testing::fixturesDir() + "/" + name, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

} // namespace

TEST_CASE("suggestions are the folders that exist, once each, marked when they hold the game") {
    testing::MemoryFileSystem files;
    files.dirs = {"/games/Orion Pirates", "/home/TestCaptain/.wine/drive_c"};
    files.files["/games/Orion Pirates/" + kExe] = "MZ";
    const std::vector<Suggestion> suggestions = suggestionsFrom(
        {"/home/TestCaptain/.wine/drive_c", "", "/nowhere", "/games/Orion Pirates",
         "/home/TestCaptain/.wine/drive_c"},
        files);
    REQUIRE(suggestions.size() == 2);
    CHECK(suggestions[0].path == "/home/TestCaptain/.wine/drive_c");
    CHECK_FALSE(suggestions[0].holdsGame);
    CHECK(suggestions[1].path == "/games/Orion Pirates");
    CHECK(suggestions[1].holdsGame);
}

TEST_CASE("the game in use is the imported copy with its record, or an install used in place") {
    testing::MemoryFileSystem files;
    const auto when = [](const std::string &path) {
        return path == kGame + "/.sfcop-import.txt" ? std::string("2026-11-12 14:03")
                                                    : std::string("wrong file");
    };
    CHECK_FALSE(gameInUseOf(kData, kGame, files, when)); // no install.txt

    files.files[kData + "/install.txt"] = "game\n";
    CHECK_FALSE(gameInUseOf(kData, kGame, files, when)); // names a copy that is not there

    files.files[kGame + "/" + kExe] = "MZ";
    files.files[kGame + "/.sfcop-import.txt"] =
        "source\t/games/Orion Pirates\nfiles\t4215\nbytes\t750312448\n";
    std::optional<GameInUse> game = gameInUseOf(kData, kGame, files, when);
    REQUIRE(game);
    CHECK(game->dir == kGame);
    CHECK(game->imported);
    CHECK(game->source == "/games/Orion Pirates");
    CHECK(game->files == 4215);
    CHECK(game->bytes == 750312448);
    CHECK(game->when == "2026-11-12 14:03");

    files.files[kData + "/install.txt"] = "/games/Orion Pirates\n";
    files.files["/games/Orion Pirates/" + kExe] = "MZ";
    game = gameInUseOf(kData, kGame, files, when);
    REQUIRE(game);
    CHECK(game->dir == "/games/Orion Pirates");
    CHECK_FALSE(game->imported);
    CHECK(game->source.empty());
    CHECK(game->when.empty());
}

TEST_CASE("the folder picker is missing only on Linux with no session bus and no zenity") {
    const auto none = [](const std::string &) { return false; };
    const auto zenity = [](const std::string &program) { return program == "zenity"; };
    CHECK(folderPickerOn(Platform::kMacos, envOf(std::nullopt), none) == FolderPicker::kAvailable);
    CHECK(folderPickerOn(Platform::kWindows, envOf(std::nullopt), none) ==
          FolderPicker::kAvailable);
    CHECK(folderPickerOn(Platform::kLinux, envOf(std::nullopt), none) ==
          FolderPicker::kUnavailable);
    CHECK(folderPickerOn(Platform::kLinux, envOf(std::string()), none) ==
          FolderPicker::kUnavailable);
    CHECK(folderPickerOn(Platform::kLinux, envOf(std::string("unix:path=/run/user/1000/bus")),
                         none) == FolderPicker::kAvailable);
    CHECK(folderPickerOn(Platform::kLinux, envOf(std::nullopt), zenity) ==
          FolderPicker::kAvailable);
}

TEST_CASE("the launcher is set up with the manifest beside the hash list") {
    testing::MemoryFileSystem files;
    files.dirs = {"/games/Orion Pirates"};
    LauncherRequest request;
    request.dataDir = kData;
    request.gameDir = kGame;
    request.hashListPath = "/sfcop/data/install.sha256";
    request.installDirs = {"/games/Orion Pirates", "/nowhere"};
    request.onDemand = true;
    request.files = &files;

    Setup setup = setupFor(request, FolderPicker::kUnavailable);
    CHECK(setup.dataDir == kData);
    CHECK(setup.gameDir == kGame);
    CHECK(setup.rememberedFile == "install.txt");
    CHECK(setup.picker == FolderPicker::kUnavailable);
    CHECK(setup.openOnReady);
    REQUIRE(setup.suggestions.size() == 1);
    CHECK(setup.suggestions[0].path == "/games/Orion Pirates");
    CHECK(setup.manifest.error == "/sfcop/data/install_manifest.tsv cannot be read");

    files.files["/sfcop/data/install_manifest.tsv"] = fixtureText("install_manifest.tsv");
    setup = setupFor(request, FolderPicker::kAvailable);
    CHECK(setup.manifest.error.empty());
    CHECK_FALSE(setup.manifest.entries.empty());
}

} // namespace sfcop::app::launcher
