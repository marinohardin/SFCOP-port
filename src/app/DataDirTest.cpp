#include "app/DataDir.h"

#include <iterator>
#include <map>
#include <string>

#include <doctest/doctest.h>

#include "app/FilesMock.h"

using namespace sfcop::app;
using sfcop::app::testing::MemoryFileSystem;

namespace {

EnvLookup envOf(std::map<std::string, std::string> values) {
    return [values](const std::string &name) -> std::optional<std::string> {
        const auto found = values.find(name);
        if (found == values.end()) {
            return std::nullopt;
        }
        return found->second;
    };
}

std::string dirOf(Platform platform, std::map<std::string, std::string> env) {
    const DataDirResult result = resolveDataDir(platform, envOf(std::move(env)), std::nullopt);
    REQUIRE(result.dir.has_value());
    return *result.dir;
}

} // namespace

TEST_CASE("the data folder's subfolders are Store's user folders, by name and in order") {
    REQUIRE(std::size(kDataSubdirs) == 5);
    CHECK(std::string(kDataSubdirs[0]) == "settings");
    CHECK(std::string(kDataSubdirs[1]) == "saves");
    CHECK(std::string(kDataSubdirs[2]) == "replays");
    CHECK(std::string(kDataSubdirs[3]) == "logs");
    CHECK(std::string(kDataSubdirs[4]) == "mods");
}

TEST_CASE("on macOS the data folder is in Application Support") {
    CHECK(dirOf(Platform::kMacos, {{"HOME", "/Users/testcaptain"},
                                   {"XDG_DATA_HOME", "/Users/testcaptain/xdg"}}) ==
          "/Users/testcaptain/Library/Application Support/SFCOP");
    CHECK(resolveDataDir(Platform::kMacos, envOf({}), std::nullopt).error ==
          "The per-user data folder cannot be found because HOME is not set. Give one with --data "
          "DIR.");
}

TEST_CASE("on Windows the data folder is in APPDATA") {
    CHECK(dirOf(Platform::kWindows, {{"APPDATA", "C:\\Users\\TestCaptain\\AppData\\Roaming"},
                                     {"HOME", "/c/Users/TestCaptain"}}) ==
          "C:\\Users\\TestCaptain\\AppData\\Roaming\\SFCOP");
    CHECK(dirOf(Platform::kWindows, {{"APPDATA", "D:\\Profiles\\"}}) == "D:\\Profiles\\SFCOP");
    CHECK(resolveDataDir(Platform::kWindows, envOf({{"APPDATA", ""}}), std::nullopt).error ==
          "The per-user data folder cannot be found because APPDATA is not set. Give one with "
          "--data DIR.");
}

TEST_CASE("on Linux the data folder follows XDG_DATA_HOME, else ~/.local/share") {
    CHECK(dirOf(Platform::kLinux, {{"HOME", "/home/testcaptain"},
                                   {"XDG_DATA_HOME", "/data/testcaptain"}}) ==
          "/data/testcaptain/SFCOP");
    CHECK(dirOf(Platform::kLinux, {{"HOME", "/home/testcaptain"}}) ==
          "/home/testcaptain/.local/share/SFCOP");
    CHECK(dirOf(Platform::kLinux, {{"HOME", "/home/testcaptain"}, {"XDG_DATA_HOME", ""}}) ==
          "/home/testcaptain/.local/share/SFCOP");
    // The XDG rules say a relative value is ignored.
    CHECK(dirOf(Platform::kLinux, {{"HOME", "/home/testcaptain"}, {"XDG_DATA_HOME", "data"}}) ==
          "/home/testcaptain/.local/share/SFCOP");
    CHECK(resolveDataDir(Platform::kLinux, envOf({{"XDG_DATA_HOME", "rel"}}), std::nullopt)
              .error == "The per-user data folder cannot be found because HOME is not set. Give "
                        "one with --data DIR.");
}

TEST_CASE("--data overrides the platform's folder on every platform") {
    for (const Platform platform : {Platform::kMacos, Platform::kWindows, Platform::kLinux}) {
        const DataDirResult result =
            resolveDataDir(platform, envOf({}), std::string("/srv/testcaptain/sfcop"));
        CHECK(result.dir == std::optional<std::string>("/srv/testcaptain/sfcop"));
    }
}

TEST_CASE("the data folder and the subfolders of DESIGN 6.6 are created on first use") {
    MemoryFileSystem files;
    CHECK(ensureDataDir("/home/testcaptain/.local/share/SFCOP", files) == "");
    CHECK(files.created == std::vector<std::string>{
                               "/home/testcaptain/.local/share/SFCOP",
                               "/home/testcaptain/.local/share/SFCOP/settings",
                               "/home/testcaptain/.local/share/SFCOP/saves",
                               "/home/testcaptain/.local/share/SFCOP/replays",
                               "/home/testcaptain/.local/share/SFCOP/logs",
                               "/home/testcaptain/.local/share/SFCOP/mods",
                           });
    MemoryFileSystem windows;
    CHECK(ensureDataDir("C:\\Users\\TestCaptain\\AppData\\Roaming\\SFCOP", windows) == "");
    CHECK(windows.created[1] == "C:\\Users\\TestCaptain\\AppData\\Roaming\\SFCOP\\settings");
}

TEST_CASE("a data folder that cannot be created is reported by name") {
    MemoryFileSystem files;
    files.refuseWrites = true;
    CHECK(ensureDataDir("/ro/SFCOP", files) ==
          "The per-user data folder /ro/SFCOP cannot be created.");
}

TEST_CASE("the install path is remembered in the data folder and read back") {
    MemoryFileSystem files;
    CHECK_FALSE(rememberedInstall("/d", files).has_value());
    CHECK(rememberInstall("/d", "/games/Orion Pirates", files));
    CHECK(files.files["/d/install.txt"] == "/games/Orion Pirates\n");
    CHECK(rememberedInstall("/d", files) == std::optional<std::string>("/games/Orion Pirates"));

    files.files["/d/install.txt"] = "C:\\Games\\OP\r\n";
    CHECK(rememberedInstall("/d", files) == std::optional<std::string>("C:\\Games\\OP"));
    files.files["/d/install.txt"] = "\n";
    CHECK_FALSE(rememberedInstall("/d", files).has_value());
}

TEST_CASE("the mission modules folder named once in the data folder is read back absolute") {
    MemoryFileSystem files;
    CHECK_FALSE(rememberedMissionModules("/d", files).has_value());
    files.files["/d/mission-modules.txt"] = "/work/missions/modules\n";
    CHECK(rememberedMissionModules("/d", files) ==
          std::optional<std::string>("/work/missions/modules"));
    files.files["/d/mission-modules.txt"] = "D:\\Missions\\modules\r\n";
    CHECK(rememberedMissionModules("/d", files) ==
          std::optional<std::string>("D:\\Missions\\modules"));
    // A relative path is taken against the data folder, not the folder the client started in.
    files.files["/d/mission-modules.txt"] = "mods/translated\n";
    CHECK(rememberedMissionModules("/d", files) ==
          std::optional<std::string>("/d/mods/translated"));
    files.files["/d/mission-modules.txt"] = "\n";
    CHECK_FALSE(rememberedMissionModules("/d", files).has_value());
    CHECK(files.written.empty()); // read only: the player writes the file
}

TEST_CASE("the imported game goes in the data folder, and on Windows in the local folder") {
    const EnvLookup env = envOf({{"LOCALAPPDATA", "C:\\Users\\tc\\AppData\\Local"},
                                 {"APPDATA", "C:\\Users\\tc\\AppData\\Roaming"}});
    CHECK(gameDirFor(Platform::kMacos, env, false, "/Users/tc/Library/Application Support/SFCOP") ==
          "/Users/tc/Library/Application Support/SFCOP/game");
    CHECK(gameDirFor(Platform::kLinux, env, false, "/home/tc/.local/share/SFCOP") ==
          "/home/tc/.local/share/SFCOP/game");
    CHECK(gameDirFor(Platform::kWindows, env, false, "C:\\Users\\tc\\AppData\\Roaming\\SFCOP") ==
          "C:\\Users\\tc\\AppData\\Local\\SFCOP\\game");
    // --data keeps everything in the folder given; without LOCALAPPDATA the data folder is used.
    CHECK(gameDirFor(Platform::kWindows, env, true, "D:\\sfcop-data") == "D:\\sfcop-data\\game");
    CHECK(gameDirFor(Platform::kWindows, envOf({}), false, "C:\\Roaming\\SFCOP") ==
          "C:\\Roaming\\SFCOP\\game");
}

TEST_CASE("install.txt's value game is the imported copy; other relative values the data folder's") {
    MemoryFileSystem files;
    CHECK_FALSE(rememberedInstallDir("/d", "/local/game", files).has_value());
    files.files["/d/install.txt"] = "game\n";
    CHECK(rememberedInstallDir("/d", "/local/game", files) ==
          std::optional<std::string>("/local/game"));
    files.files["/d/install.txt"] = "/games/Orion Pirates\n";
    CHECK(rememberedInstallDir("/d", "/local/game", files) ==
          std::optional<std::string>("/games/Orion Pirates"));
    files.files["/d/install.txt"] = "older/copy\n";
    CHECK(rememberedInstallDir("/d", "/local/game", files) ==
          std::optional<std::string>("/d/older/copy"));
}
