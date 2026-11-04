// The .ini profile calls: read where the game sees the profile, written to its per-user copy.

#include "hle/kernel/Kernel.h"
#include "hle/kernel/KernelTestFolders.h"

#include <doctest/doctest.h>

#include <cstring>

namespace sfcop::hle::kernel {
namespace {

using test::GameFolders;
using namespace win32;

const char kProfile[] = "; test profile\r\n"
                        "[Display]\r\n"
                        "  Width = 1024  \r\n"
                        "Title=\"Test Game\"\r\n"
                        "Depth=0x10\r\n"
                        "Neg=-7\r\n"
                        "Partly=12abc\r\n"
                        "Word=abc\r\n"
                        "Empty=\r\n"
                        "\r\n"
                        "[Sound]\r\n"
                        "Volume=80\r\n";

std::string get(Kernel &kernel, const char *section, const char *key, const char *fallback,
                std::uint32_t size = 64, const char *file = "C:\\test.ini") {
    char buffer[64];
    std::memset(buffer, '#', sizeof buffer);
    std::uint32_t count = kernel.GetPrivateProfileString(section, key, fallback, buffer, size, file);
    return std::string(buffer, count);
}

TEST_CASE("a profile value is found without case, trimmed and unquoted") {
    GameFolders game;
    game.InstallFile("test.ini", kProfile);
    Kernel &kernel = game.kernel;
    CHECK(get(kernel, "display", "WIDTH", "x") == "1024");
    CHECK(get(kernel, "Display", "Title", "x") == "Test Game");
    CHECK(get(kernel, "Sound", "Volume", "x") == "80");
    CHECK(get(kernel, "Display", "Empty", "x") == "");
    CHECK(get(kernel, "Display", "Volume", "fallback  ") == "fallback");
    CHECK(get(kernel, "Missing", "Width", nullptr) == "");
    CHECK(get(kernel, "Display", "Width", "x", 3) == "10");
}

TEST_CASE("a null section lists the sections and a null key the keys, double-NUL ended") {
    GameFolders game;
    game.InstallFile("test.ini", kProfile);
    Kernel &kernel = game.kernel;
    CHECK(get(kernel, nullptr, nullptr, "x") == std::string("Display\0Sound\0", 14));
    CHECK(get(kernel, "Sound", nullptr, "x") == std::string("Volume\0", 7));
    char cut[8];
    CHECK(kernel.GetPrivateProfileString(nullptr, nullptr, "", cut, sizeof cut, "C:\\test.ini") == 6);
    CHECK(std::memcmp(cut, "Displa\0\0", 8) == 0);
}

TEST_CASE("a missing profile answers the default with ERROR_FILE_NOT_FOUND") {
    GameFolders game;
    game.InstallFile("test.ini", kProfile);
    CHECK(get(game.kernel, "Display", "Width", "640", 64, "C:\\none.ini") == "640");
    CHECK(game.kernel.TakeLastError() == kErrorFileNotFound);
    // A bare name is in the Windows directory on Windows; here it is the per-user copy, else the
    // install's game folder.
    CHECK(get(game.kernel, "Display", "Width", "640", 64, "test.ini") == "1024");
    CHECK(get(game.kernel, "Display", "Width", "640", 64, "other.ini") == "640");
}

TEST_CASE("a profile number is decimal with an optional sign, and the default when absent") {
    GameFolders game;
    game.InstallFile("test.ini", kProfile);
    Kernel &kernel = game.kernel;
    CHECK(kernel.GetPrivateProfileInt("Display", "Width", 5, "C:\\test.ini") == 1024);
    CHECK(kernel.GetPrivateProfileInt("Display", "Depth", 5, "C:\\test.ini") == 0); // "0x10"
    CHECK(kernel.GetPrivateProfileInt("Display", "Neg", 5, "C:\\test.ini") == 0xFFFFFFF9u);
    CHECK(kernel.GetPrivateProfileInt("Display", "Partly", 5, "C:\\test.ini") == 12);
    CHECK(kernel.GetPrivateProfileInt("Display", "Word", 5, "C:\\test.ini") == 0);
    CHECK(kernel.GetPrivateProfileInt("Display", "Empty", 5, "C:\\test.ini") == 5);
    CHECK(kernel.GetPrivateProfileInt("Display", "None", -2, "C:\\test.ini") == 0xFFFFFFFEu);
}

TEST_CASE("writing a profile changes its per-user copy in place and leaves the install") {
    GameFolders game;
    game.InstallFile("test.ini", kProfile);
    Kernel &kernel = game.kernel;
    CHECK(kernel.WritePrivateProfileString("display", "width", "800", "C:\\test.ini"));
    CHECK(kernel.WritePrivateProfileString("Display", "Gamma", "2", "C:\\test.ini"));
    CHECK(kernel.WritePrivateProfileString("Sound", "Volume", nullptr, "C:\\test.ini"));
    CHECK(kernel.WritePrivateProfileString("Network", "Port", "2300", "C:\\test.ini"));
    CHECK(game.InstallContent("test.ini") == kProfile);
    CHECK(game.UserContent("settings/test.ini") == "; test profile\r\n"
                                                   "[Display]\r\n"
                                                   "width=800\r\n"
                                                   "Title=\"Test Game\"\r\n"
                                                   "Depth=0x10\r\n"
                                                   "Neg=-7\r\n"
                                                   "Partly=12abc\r\n"
                                                   "Word=abc\r\n"
                                                   "Empty=\r\n"
                                                   "Gamma=2\r\n"
                                                   "\r\n"
                                                   "[Sound]\r\n"
                                                   "[Network]\r\n"
                                                   "Port=2300\r\n");
    CHECK(get(kernel, "Display", "Width", "x") == "800");
    CHECK(get(kernel, "Network", "Port", "x") == "2300");

    CHECK(kernel.WritePrivateProfileString("Display", nullptr, nullptr, "C:\\test.ini"));
    CHECK(get(kernel, nullptr, nullptr, "x") == std::string("Sound\0Network\0", 14));
}

TEST_CASE("a new profile is created in its per-user folder; a missing folder is refused") {
    GameFolders game;
    game.InstallFile("Assets/Specs/bindings.ini", "");
    Kernel &kernel = game.kernel;
    CHECK(kernel.WritePrivateProfileString("Keys", "Fire", "Space", "C:\\Assets\\Specs\\new.ini"));
    CHECK(game.UserContent("settings/Assets/Specs/new.ini") == "[Keys]\r\nFire=Space\r\n");
    CHECK_FALSE(kernel.WritePrivateProfileString("Keys", "Fire", "Space", "C:\\None\\new.ini"));
    CHECK(kernel.TakeLastError() == kErrorPathNotFound);
    CHECK_FALSE(kernel.WritePrivateProfileString(nullptr, "Fire", "Space", "C:\\x.ini"));
    CHECK(kernel.TakeLastError() == kErrorInvalidParameter);
    CHECK(kernel.WritePrivateProfileString(nullptr, nullptr, nullptr, "C:\\x.ini"));
}

TEST_CASE("a bare profile name is written to windows/ under the per-user settings and read back") {
    GameFolders game;
    Kernel &kernel = game.kernel;
    CHECK(kernel.WritePrivateProfileString("Keys", "Fire", "Space", "bare.ini"));
    CHECK(kernel.WritePrivateProfileString("Keys", "Jump", "Up", "bare.ini"));
    CHECK(game.UserContent("settings/windows/bare.ini") == "[Keys]\r\nFire=Space\r\nJump=Up\r\n");
    CHECK_FALSE(game.InstallHas("bare.ini"));
    CHECK(get(kernel, "Keys", "Fire", "none", 64, "bare.ini") == "Space");
    CHECK(get(kernel, "Keys", "Jump", "none", 64, "BARE.INI") == "Up");
    // The same name with a folder is another file.
    CHECK(get(kernel, "Keys", "Fire", "none", 64, "C:\\bare.ini") == "none");
}

TEST_CASE("a bare profile name that the install has is changed in a per-user copy") {
    GameFolders game;
    game.InstallFile("sfc.ini", "[Net]\r\nPort=2300\r\n");
    Kernel &kernel = game.kernel;
    CHECK(kernel.GetPrivateProfileInt("Net", "Port", 1, "sfc.ini") == 2300);
    CHECK(kernel.WritePrivateProfileString("Net", "Port", "2400", "sfc.ini"));
    CHECK(game.InstallContent("sfc.ini") == "[Net]\r\nPort=2300\r\n");
    CHECK(game.UserContent("settings/windows/sfc.ini") == "[Net]\r\nPort=2400\r\n");
    CHECK(kernel.GetPrivateProfileInt("Net", "Port", 1, "sfc.ini") == 2400);
}

} // namespace
} // namespace sfcop::hle::kernel
