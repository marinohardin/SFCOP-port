#include "app/Startup.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include <doctest/doctest.h>

#include "app/FilesMock.h"
#include "fixes/fixes.h"

using namespace sfcop::app;
using sfcop::app::testing::fixturesDir;
using sfcop::app::testing::MemoryFileSystem;

namespace {

namespace fs = std::filesystem;

const char kExeHash[] = "a8efff83a244d64032d614355c235f36cfc10808403a6e1b00f6c18265c36cdc";

// A fresh folder under the system's temporary folder, removed afterwards.
class ScratchDir {
public:
    explicit ScratchDir(const char *name) : path_(fs::temp_directory_path() / name) {
        std::error_code error;
        fs::remove_all(path_, error);
        fs::create_directories(path_);
    }
    ~ScratchDir() {
        std::error_code error;
        fs::remove_all(path_, error);
    }
    std::string str() const { return path_.string(); }

private:
    fs::path path_;
};

// Runs startClient on Linux rules, with HOME set to `home` and the fixture's hash list.
struct Run {
    std::ostringstream out;
    std::ostringstream err;
    Startup result;

    Run(std::vector<std::string> args, const std::string &home, FileSystem *files = nullptr,
        std::function<std::optional<std::string>()> ask = nullptr,
        std::string hashList = fixturesDir() + "/install.sha256") {
        std::vector<const char *> argv = {"sfcop"};
        for (const std::string &arg : args) {
            argv.push_back(arg.c_str());
        }
        StartupConfig config;
        config.hashListPath = std::move(hashList);
        config.extraFlags = {{"--game", "DIR", "a writable copy"}};
        config.platform = Platform::kLinux;
        config.env = [home](const std::string &name) -> std::optional<std::string> {
            if (name == "HOME") {
                return home;
            }
            return std::nullopt;
        };
        config.files = files;
        config.ask = std::move(ask);
        config.out = &out;
        config.err = &err;
        result = startClient(static_cast<int>(argv.size()), argv.data(), config);
    }
};

void flipByte(const std::string &path, std::streamoff offset) {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    REQUIRE(file.is_open());
    file.seekg(offset);
    char byte = 0;
    file.get(byte);
    file.seekp(offset);
    file.put(static_cast<char>(byte ^ 0x20));
}

} // namespace

TEST_CASE("startup prints the help and stops with 0") {
    const Run run({"--help"}, "/home/testcaptain");
    CHECK(run.result.exitCode == std::optional<int>(0));
    CHECK(run.out.str().rfind("usage: sfcop [options]\n", 0) == 0);
    CHECK(run.out.str().find("  --game DIR ") != std::string::npos);
    CHECK(run.err.str().empty());
}

TEST_CASE("startup stops with 2 on an unknown flag, naming it") {
    const Run run({"--bogus"}, "/home/testcaptain");
    CHECK(run.result.exitCode == std::optional<int>(2));
    CHECK(run.err.str() == "sfcop: unknown flag '--bogus'\nRun sfcop --help to see the flags.\n");
}

TEST_CASE("startup accepts the fixture install, creates the data folder and remembers the "
          "install") {
    const ScratchDir home("sfcop-c4-startup-accept");
    const std::string install = fixturesDir() + "/install";
    const std::string data = home.str() + "/.local/share/SFCOP";
    {
        const Run run({"--install", install, "--headless", "--game", "/copy"}, home.str());
        CHECK(run.err.str() == "");
        CHECK_FALSE(run.result.exitCode.has_value());
        CHECK(run.result.installDir == install);
        CHECK(run.result.dataDir == data);
        CHECK(run.result.options.headless);
        CHECK(run.result.options.passThrough == std::vector<std::string>{"--game", "/copy"});
        for (const char *sub : {"settings", "saves", "replays", "logs", "mods"}) {
            CAPTURE(sub);
            CHECK(fs::is_directory(data + "/" + sub));
        }
        CHECK(hostFileSystem().readText(data + "/install.txt") ==
              std::optional<std::string>(install + "\n"));
    }
    // The next run finds the install it remembered.
    const Run again({"--headless"}, home.str());
    CHECK(again.err.str() == "");
    CHECK_FALSE(again.result.exitCode.has_value());
    CHECK(again.result.installDir == install);
}

TEST_CASE("startup refuses a copy of the fixture install with one byte altered, naming the file") {
    const ScratchDir home("sfcop-c4-startup-altered");
    const std::string copy = home.str() + "/op";
    fs::copy(fixturesDir() + "/install", copy, fs::copy_options::recursive);
    flipByte(copy + "/Assets/Models/test_frigate.mod", 7);

    const Run run({"--install", copy, "--headless"}, home.str());
    CHECK(run.result.exitCode == std::optional<int>(3));
    CHECK(run.err.str() == "sfcop: Assets/Models/test_frigate.mod in the install at " + copy +
                               " differs from the 2.5.5.2 release. Reinstall Starfleet Command: "
                               "Orion Pirates from its disc and apply the official 2.5.5.2 patch, "
                               "then start sfcop again.\n");
    // A refused install is not remembered.
    CHECK_FALSE(fs::exists(home.str() + "/.local/share/SFCOP/install.txt"));
}

TEST_CASE("startup stops with 3 when no install is found, and asks only when not headless") {
    MemoryFileSystem files;
    files.files["/list.sha256"] = std::string(kExeHash) + "  StarFleetOP.exe\n";
    int asked = 0;
    const auto ask = [&asked]() -> std::optional<std::string> {
        ++asked;
        return std::nullopt;
    };
    const Run headless({"--headless"}, "/home/testcaptain", &files, ask, "/list.sha256");
    CHECK(asked == 0);
    CHECK(headless.result.exitCode == std::optional<int>(3));
    CHECK(headless.err.str() ==
          "sfcop: No install of Starfleet Command: Orion Pirates was found. Give its folder, the "
          "one that holds StarFleetOP.exe, with --install DIR.\n");

    const Run windowed({}, "/home/testcaptain", &files, ask, "/list.sha256");
    CHECK(asked == 1);
    CHECK(windowed.result.exitCode == std::optional<int>(3));
}

TEST_CASE("startup stops with 3 when the hash list is unreadable or malformed") {
    MemoryFileSystem files;
    const Run absent({"--headless"}, "/home/testcaptain", &files, nullptr, "/list.sha256");
    CHECK(absent.result.exitCode == std::optional<int>(3));
    CHECK(absent.err.str() == "sfcop: the hash list /list.sha256 cannot be read, so the install "
                              "cannot be verified.\n");

    files.files["/list.sha256"] = std::string(kExeHash) + "  StarFleetOP.exe\nbroken line\n";
    const Run broken({"--headless"}, "/home/testcaptain", &files, nullptr, "/list.sha256");
    CHECK(broken.result.exitCode == std::optional<int>(3));
    CHECK(broken.err.str() == "sfcop: the hash list /list.sha256, line 2: expected 64 hex digits "
                              "of SHA-256 at the start, so the install cannot be verified.\n");
}

TEST_CASE("startup stops with 1 when the data folder cannot be created") {
    MemoryFileSystem files;
    files.refuseWrites = true;
    const Run run({"--data", "/ro/SFCOP"}, "/home/testcaptain", &files);
    CHECK(run.result.exitCode == std::optional<int>(1));
    CHECK(run.err.str() == "sfcop: The per-user data folder /ro/SFCOP cannot be created.\n");
}

TEST_CASE("startup makes a relative install and data folder absolute before remembering them") {
    MemoryFileSystem files;
    files.currentDir = "/home/testcaptain/games";
    files.files["/home/testcaptain/games/op/StarFleetOP.exe"] = "x";
    // The SHA-256 of "x".
    files.files["/list.sha256"] =
        "2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881  StarFleetOP.exe\n";
    const Run run({"--install", "op", "--data", "sfcop-data", "--headless"}, "/home/testcaptain",
                  &files, nullptr, "/list.sha256");
    CHECK(run.err.str() == "");
    CHECK(run.result.installDir == "/home/testcaptain/games/op");
    CHECK(run.result.dataDir == "/home/testcaptain/games/sfcop-data");
    CHECK(files.files["/home/testcaptain/games/sfcop-data/install.txt"] ==
          "/home/testcaptain/games/op\n");
}

TEST_CASE("startup tries the caller's install folders before the platform's usual places") {
    MemoryFileSystem files;
    files.files["/list.sha256"] = std::string(kExeHash) + "  StarFleetOP.exe\n";
    files.files["/build/op/StarFleetOP.exe"] = "x";
    files.files["/home/testcaptain/.wine/drive_c/Program Files/Taldren/Starfleet Command Orion "
                "Pirates/StarFleetOP.exe"] = "x";
    std::ostringstream out;
    std::ostringstream err;
    StartupConfig config = consoleStartup("/list.sha256", {}, {"/build/op"});
    CHECK(config.ask != nullptr);
    config.platform = Platform::kLinux;
    config.env = [](const std::string &name) -> std::optional<std::string> {
        if (name == "HOME") {
            return std::string("/home/testcaptain");
        }
        return std::nullopt;
    };
    config.files = &files;
    config.out = &out;
    config.err = &err;
    const char *argv[] = {"sfcop", "--headless"};
    const Startup result = startClient(2, argv, config);
    // Found, then refused: the stand-in "x" is not the listed executable.
    CHECK(result.exitCode == std::optional<int>(3));
    CHECK(err.str().rfind("sfcop: StarFleetOP.exe in the install at /build/op differs", 0) == 0);
}

// An install of one file, "x", and a data folder, in memory; `fixesFile` is settings/fixes.gf when
// it is given.
struct FixRun {
    MemoryFileSystem files;
    std::optional<Run> run;

    explicit FixRun(std::vector<std::string> args,
                    std::optional<std::string> fixesFile = std::nullopt) {
        files.files["/games/op/StarFleetOP.exe"] = "x";
        // The SHA-256 of "x".
        files.files["/list.sha256"] =
            "2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881  StarFleetOP.exe\n";
        if (fixesFile) {
            files.files["/sfcop-data/settings/fixes.gf"] = *fixesFile;
        }
        std::vector<std::string> all = {"--install", "/games/op", "--data", "/sfcop-data",
                                        "--headless"};
        all.insert(all.end(), args.begin(), args.end());
        run.emplace(all, "/home/testcaptain", &files, nullptr, "/list.sha256");
    }
    ~FixRun() { sfcop::fixes::install(sfcop::fixes::Settings()); }
};

TEST_CASE("startup installs each --fix, so isEnabled answers the command line's position") {
    const FixRun fix({"--fix", "fix_smooth_scaled_frame=off"});
    CHECK(fix.run->err.str() == "");
    CHECK_FALSE(fix.run->result.exitCode.has_value());
    CHECK_FALSE(sfcop::fixes::isEnabled("fix_smooth_scaled_frame"));
    CHECK(sfcop::fixes::current().sourceOf("fix_smooth_scaled_frame") ==
          sfcop::fixes::Source::CommandLine);
    CHECK(sfcop::fixes::isEnabled("fix_hle_named_objects"));
}

TEST_CASE("startup applies settings/fixes.gf in the data folder, and --fix wins over it") {
    {
        const FixRun fix({}, "// the player's fixes\nfix_smooth_scaled_frame = off\n");
        CHECK(fix.run->err.str() == "");
        CHECK_FALSE(sfcop::fixes::isEnabled("fix_smooth_scaled_frame"));
        CHECK(sfcop::fixes::current().sourceOf("fix_smooth_scaled_frame") ==
              sfcop::fixes::Source::File);
    }
    const FixRun fix({"--fix", "fix_smooth_scaled_frame=on"}, "fix_smooth_scaled_frame = off\n");
    CHECK(fix.run->err.str() == "");
    CHECK(sfcop::fixes::isEnabled("fix_smooth_scaled_frame"));
    CHECK(sfcop::fixes::current().sourceOf("fix_smooth_scaled_frame") ==
          sfcop::fixes::Source::CommandLine);
}

TEST_CASE("startup leaves the fixes at their defaults when no file and no --fix is given") {
    const FixRun fix({});
    CHECK(fix.run->err.str() == "");
    for (const sfcop::fixes::Switch &declared : sfcop::fixes::registry()) {
        CAPTURE(declared.name);
        CHECK(sfcop::fixes::isEnabled(declared.name) == declared.defaultOn);
        CHECK(sfcop::fixes::current().sourceOf(declared.name) == sfcop::fixes::Source::Default);
    }
}

TEST_CASE("startup prints each unknown fix once, from the file and from --fix, and still starts") {
    const FixRun fix({"--fix", "fix_nonesuch=on", "--fix", "fix_smooth_scaled_frame=off"},
                     "fix_smooth_scaled_frame = on\nfix_unheard_of = off\n");
    CHECK(fix.run->err.str() ==
          "sfcop: /sfcop-data/settings/fixes.gf, line 2: unknown fix switch 'fix_unheard_of'; it "
          "is ignored.\n"
          "sfcop: --fix: unknown fix switch 'fix_nonesuch'; it is ignored.\n");
    CHECK_FALSE(fix.run->result.exitCode.has_value());
    CHECK_FALSE(sfcop::fixes::isEnabled("fix_smooth_scaled_frame"));
}

TEST_CASE("startup says so when settings/fixes.gf exists but cannot be read, and starts") {
    MemoryFileSystem files;
    files.files["/games/op/StarFleetOP.exe"] = "x";
    files.files["/list.sha256"] =
        "2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881  StarFleetOP.exe\n";
    files.files["/sfcop-data/settings/fixes.gf"] = "fix_smooth_scaled_frame = off\n";
    // fileSize sees the file; readText cannot read it.
    struct Unreadable : FileSystem {
        MemoryFileSystem &inner;
        explicit Unreadable(MemoryFileSystem &files) : inner(files) {}
        bool isDirectory(const std::string &path) const override {
            return inner.isDirectory(path);
        }
        std::optional<std::uint64_t> fileSize(const std::string &path) const override {
            return inner.fileSize(path);
        }
        std::unique_ptr<FileReader> openRead(const std::string &path) const override {
            return inner.openRead(path);
        }
        bool createDirectories(const std::string &path) override {
            return inner.createDirectories(path);
        }
        std::optional<std::string> readText(const std::string &path) const override {
            if (path == "/sfcop-data/settings/fixes.gf") {
                return std::nullopt;
            }
            return inner.readText(path);
        }
        bool writeTextAtomic(const std::string &path, const std::string &text) override {
            return inner.writeTextAtomic(path, text);
        }
        std::string absolutePath(const std::string &path) const override {
            return inner.absolutePath(path);
        }
    } unreadable(files);
    const Run run({"--install", "/games/op", "--data", "/sfcop-data", "--headless"},
                  "/home/testcaptain", &unreadable, nullptr, "/list.sha256");
    CHECK(run.err.str() ==
          "sfcop: /sfcop-data/settings/fixes.gf cannot be read; the fixes keep their defaults.\n");
    CHECK_FALSE(run.result.exitCode.has_value());
    CHECK(sfcop::fixes::isEnabled("fix_smooth_scaled_frame"));
}

TEST_CASE("the console asks once and reads one line, dropping quotes and spaces") {
    std::istringstream in("  \"/games/Orion Pirates\"  \n\n");
    std::ostringstream out;
    const auto ask = askOnConsole(in, out);
    CHECK(ask() == std::optional<std::string>("/games/Orion Pirates"));
    CHECK(out.str() == "Where is Starfleet Command: Orion Pirates installed? Give the folder that "
                       "holds StarFleetOP.exe: ");
    CHECK_FALSE(ask().has_value());
    CHECK_FALSE(ask().has_value());
}
