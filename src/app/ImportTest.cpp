#include "app/Import.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <doctest/doctest.h>

#include "app/DataDir.h"
#include "app/ImportCommand.h"
#include "app/ImportFilesMock.h"

using namespace sfcop::app;
using sfcop::app::testing::ControlledImportFiles;

namespace {

namespace fs = std::filesystem;

// Makes a link, or says why not: Windows refuses links to a user without Developer Mode, and the
// tests that need one are then skipped rather than failed.
bool makeLink(const std::string &target, const std::string &link, bool folder) {
    std::error_code error;
    if (folder) {
        fs::create_directory_symlink(target, link, error);
    } else {
        fs::create_symlink(target, link, error);
    }
    if (error) {
        MESSAGE("skipped: this system will not make a link (" << error.message() << ")");
    }
    return !error;
}

// The manifest of the invented install below. Sizes and hashes were worked out by hand
// (printf ... | shasum -a 256) from the contents written by writeGoodInstall.
const char kManifest[] =
    "# an invented manifest\n"
    "StarFleetOP.exe\t21\tea717358f9e6b59cdb5ff1c2a8e6a51a313a7bfc5aaa3c2fc464664712668bad\n"
    "Assets/Models/test.mod\t15\te4abd5e0a853321dba13a23ae9cc5819f03dda33fb74a5c432b3a9b56350f15f\n"
    "Assets/Scripts/Patrol.scr\t16\ta0cb1951d546092638c6dacc7bd8088c330367697202063e264bf4520532144c\n"
    "\n"
    "MetaAssets/shiplist.txt\t15\ta39ee55cda3f9e9c672e7fa5d424409bfc467c314d921991284ce80b2ea48e29\n"
    "sfc.ini\t16\teb35d2d300c0a1b9261b326c0b71f84128cdde375ce32957add4540cdf9e7d02\n";

// A fresh folder under the system's temporary folder, removed afterwards.
class Scratch {
public:
    explicit Scratch(const char *name) : root_(fs::temp_directory_path() / name) {
        std::error_code error;
        fs::remove_all(root_, error);
        fs::create_directories(root_);
    }
    ~Scratch() {
        std::error_code error;
        fs::remove_all(root_, error);
    }
    Scratch(const Scratch &) = delete;
    Scratch &operator=(const Scratch &) = delete;

    std::string path(const std::string &relative = {}) const {
        return relative.empty() ? root_.string() : (root_ / relative).string();
    }

    void write(const std::string &relative, const std::string &bytes) const {
        const fs::path file = root_ / relative;
        fs::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << bytes;
    }

    // A file of `size` zero bytes.
    void writeSized(const std::string &relative, std::uint64_t size) const {
        write(relative, std::string());
        fs::resize_file(root_ / relative, size);
    }

    bool exists(const std::string &relative) const { return fs::exists(root_ / relative); }

    std::string read(const std::string &relative) const {
        std::ifstream in(root_ / relative, std::ios::binary);
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }

private:
    fs::path root_;
};

// The invented 2.5.5.2 install under `dir`, plus two files that are not in the import set.
void writeGoodInstall(const Scratch &s, const std::string &dir = "install") {
    s.write(dir + "/StarFleetOP.exe", "invented exe 2.5.5.2\n");
    s.write(dir + "/Assets/Models/test.mod", "invented model\n");
    s.write(dir + "/Assets/Scripts/Patrol.scr", "invented script\n");
    s.write(dir + "/MetaAssets/shiplist.txt", "invented ships\n");
    s.write(dir + "/sfc.ini", "[3D]\nwindowed=0\n");
    s.write(dir + "/Readme.txt", "left out\n");
    s.write(dir + "/.DS_Store", "left out\n");
}

ImportRequest requestFor(const Scratch &s, const std::string &source = "install") {
    ImportRequest request;
    request.source = s.path(source);
    request.gameDir = s.path("data/game");
    request.manifest = parseInstallManifest(kManifest);
    REQUIRE(request.manifest.ok());
    return request;
}

} // namespace

TEST_CASE("the install manifest is read: path, size and SHA-256 per line, comments skipped") {
    const InstallManifest manifest = parseInstallManifest(kManifest);
    REQUIRE(manifest.ok());
    REQUIRE(manifest.entries.size() == 5);
    CHECK(manifest.entries[1].path == "Assets/Models/test.mod");
    CHECK(manifest.entries[1].size == 15);
    CHECK(manifest.entries[1].sha256 ==
          "e4abd5e0a853321dba13a23ae9cc5819f03dda33fb74a5c432b3a9b56350f15f");
    REQUIRE(manifest.exe() != nullptr);
    CHECK(manifest.exe()->size == 21);
}

TEST_CASE("a malformed install manifest is refused with its line number") {
    const std::string hash = "ea717358f9e6b59cdb5ff1c2a8e6a51a313a7bfc5aaa3c2fc464664712668bad";
    const std::string exe = "StarFleetOP.exe\t21\t" + hash + "\n";
    CHECK(parseInstallManifest(exe + "a.mod 15 " + hash + "\n").error ==
          "line 2: expected a path, a size and a SHA-256, separated by tabs");
    CHECK(parseInstallManifest(exe + "../a.mod\t15\t" + hash + "\n").error ==
          "line 2: the path '../a.mod' must be relative, with '/' between parts, and stay "
          "inside the root");
    CHECK(parseInstallManifest(exe + "a.mod\t1.5\t" + hash + "\n").error ==
          "line 2: the size '1.5' is not a whole number");
    CHECK(parseInstallManifest(exe + "a.mod\t15\tabc\n").error ==
          "line 2: expected 64 hex digits of SHA-256");
    CHECK(parseInstallManifest(exe + "A.MOD\t15\t" + hash + "\na.mod\t15\t" + hash + "\n").error ==
          "line 3: 'a.mod' is listed twice (first on line 2)");
    CHECK(parseInstallManifest("a.mod\t15\t" + hash + "\n").error ==
          "the manifest names no StarFleetOP.exe at its root");
}

TEST_CASE("the install manifest sits beside the hash list") {
    CHECK(installManifestBeside("/src/analysis/results/retail_hashes.sha256") ==
          "/src/analysis/results/install_manifest.tsv");
    CHECK(installManifestBeside("retail_hashes.sha256") == "install_manifest.tsv");
}

TEST_CASE("a good install is copied into the game folder, verified, with a record") {
    const Scratch s("sfcop-l2a-good");
    writeGoodInstall(s);
    ControlledImportFiles files;
    std::vector<ImportPhase> phases;
    std::uint64_t lastDone = 0;
    std::uint64_t total = 0;
    ImportCallbacks callbacks;
    callbacks.progress = [&](const ImportProgress &p) {
        if (phases.empty() || phases.back() != p.phase) {
            phases.push_back(p.phase);
        }
        lastDone = p.bytesDone;
        total = p.bytesTotal;
    };

    const ImportReport report = importGame(requestFor(s), files, callbacks);

    REQUIRE(report.imported());
    CHECK(report.exitCode() == 0);
    CHECK(report.installDir == s.path("install"));
    CHECK(report.filesCopied == 5);
    CHECK(report.bytesCopied == 83);
    CHECK(report.changed.empty());
    CHECK(report.modScripts.empty());
    CHECK(phases == std::vector<ImportPhase>{ImportPhase::kSearching, ImportPhase::kCheckingGame,
                                              ImportPhase::kCopying, ImportPhase::kFinishing});
    CHECK(total == 83);
    CHECK(lastDone == 83);
    CHECK(s.read("data/game/Assets/Models/test.mod") == "invented model\n");
    CHECK(s.read("data/game/sfc.ini") == "[3D]\nwindowed=0\n");
    CHECK_FALSE(s.exists("data/game/Readme.txt"));
    CHECK_FALSE(s.exists("data/game/.DS_Store"));
    CHECK_FALSE(s.exists("data/game.partial"));
    CHECK(s.read("data/game/.sfcop-import.txt") ==
          "# What sfcop --import copied here. Not read by the game.\n"
          "manifest-files\t5\n"
          "source\t" + s.path("install") + "\n"
          "files\t5\n"
          "bytes\t83\n");
    CHECK(report.message() == "Imported 5 files (0.0 MB) from " + s.path("install") + " into " +
                                  s.path("data/game") + ".");
}

TEST_CASE("a clone is hashed as read back from where it lands") {
    const Scratch s("sfcop-l2a-clone");
    writeGoodInstall(s);
    ControlledImportFiles files;
    files.canClone = true;
    const ImportReport report = importGame(requestFor(s), files);
    REQUIRE(report.imported());
    CHECK(files.clones == 5);
    CHECK(report.filesCloned == 5);
    CHECK(files.writes == 1); // nothing copied through a writer: only the record
    CHECK(s.read("data/game/MetaAssets/shiplist.txt") == "invented ships\n");
}

TEST_CASE("the install is found below a Wine prefix, and the search is bounded") {
    const Scratch s("sfcop-l2a-prefix");
    writeGoodInstall(s, "prefix/drive_c/Program Files (x86)/Maker/Game");
    s.write("prefix/drive_c/windows/notepad.txt", "not the game\n");
    if (!makeLink(s.path(), s.path("prefix/dosdevices-z"), true)) {
        return;
    }
    ControlledImportFiles files;
    const ImportReport found = importGame(requestFor(s, "prefix"), files);
    REQUIRE(found.imported());
    CHECK(found.installDir == s.path("prefix/drive_c/Program Files (x86)/Maker/Game"));

    // Nine levels down is beyond the search; a folder reached only through a link is never
    // looked in.
    const Scratch deep("sfcop-l2a-deep");
    writeGoodInstall(deep, "a/b/c/d/e/f/g/h/i");
    if (!makeLink(deep.path("a/b/c/d/e/f/g/h/i"), deep.path("link"), true)) {
        return;
    }
    const ImportReport missed = importGame(requestFor(deep, ""), files);
    CHECK(missed.status == ImportStatus::kNotFound);
    CHECK(missed.exitCode() == 3);
    CHECK(missed.message() == "No StarFleetOP.exe was found in " + deep.path() +
                                  " or in the folders below it. Choose the folder Starfleet "
                                  "Command: Orion Pirates is installed in.");
}

TEST_CASE("a chosen folder that is a link, and links inside the install, are followed") {
    const Scratch s("sfcop-l2a-links");
    writeGoodInstall(s, "real");
    // The install's Assets and sfc.ini live elsewhere and are linked in, as some set-ups do.
    fs::rename(s.path("real/Assets"), s.path("elsewhere-assets"));
    if (!makeLink(s.path("elsewhere-assets"), s.path("real/Assets"), true)) {
        return;
    }
    fs::rename(s.path("real/sfc.ini"), s.path("elsewhere.ini"));
    REQUIRE(makeLink(s.path("elsewhere.ini"), s.path("real/sfc.ini"), false));
    REQUIRE(makeLink(s.path("real"), s.path("chosen"), true));
    ControlledImportFiles files;
    const ImportReport report = importGame(requestFor(s, "chosen"), files);
    REQUIRE(report.imported());
    CHECK(report.installDir == s.path("chosen"));
    CHECK(report.filesCopied == 5);
    CHECK(fs::is_regular_file(fs::symlink_status(s.path("data/game/sfc.ini"))));
    CHECK(s.read("data/game/sfc.ini") == "[3D]\nwindowed=0\n");
    CHECK(fs::is_directory(fs::symlink_status(s.path("data/game/Assets"))));
    CHECK(s.read("data/game/Assets/Models/test.mod") == "invented model\n");
}

TEST_CASE("the 2.5.5.2 install is chosen when a prefix holds an older one first") {
    const Scratch s("sfcop-l2a-two");
    s.writeSized("prefix/A/StarFleetOP.exe", kExeSize2500);
    writeGoodInstall(s, "prefix/B");
    ControlledImportFiles files;
    const ImportReport report = importGame(requestFor(s, "prefix"), files);
    REQUIRE(report.imported());
    CHECK(report.installDir == s.path("prefix/B"));
}

TEST_CASE("an executable of 2.5.0.0's size is refused with the patch to apply") {
    const Scratch s("sfcop-l2a-2500");
    writeGoodInstall(s);
    s.writeSized("install/StarFleetOP.exe", kExeSize2500);
    ControlledImportFiles files;
    const ImportReport report = importGame(requestFor(s), files);
    CHECK(report.status == ImportStatus::kVersion2500);
    CHECK(report.exitCode() == 3);
    CHECK(report.message() == s.path("install/StarFleetOP.exe") +
                                  " is version 2.5.0.0. Apply the official 2.5.5.2 patch to that "
                                  "install, then import it again.");
    CHECK(files.changes == 0);
}

TEST_CASE("an executable of 2.5.5.2's size with another hash is refused as modified") {
    const Scratch s("sfcop-l2a-modified");
    writeGoodInstall(s);
    s.write("install/StarFleetOP.exe", "invented exe 2.5.5.X\n");
    ControlledImportFiles files;
    const ImportReport report = importGame(requestFor(s), files);
    CHECK(report.status == ImportStatus::kExeModified);
    CHECK(report.message() == s.path("install/StarFleetOP.exe") +
                                  " has been modified. Restore the 2.5.5.2 StarFleetOP.exe "
                                  "(reinstall and apply the official 2.5.5.2 patch), then import "
                                  "again.");
    CHECK(files.changes == 0);
}

TEST_CASE("an executable of any other size is not the game") {
    const Scratch s("sfcop-l2a-other");
    writeGoodInstall(s);
    s.write("install/StarFleetOP.exe", "some other program\n");
    ControlledImportFiles files;
    const ImportReport report = importGame(requestFor(s), files);
    CHECK(report.status == ImportStatus::kNotTheGame);
    CHECK(report.message() ==
          s.path("install/StarFleetOP.exe") +
              " is not Starfleet Command: Orion Pirates 2.5.5.2. Install Starfleet Command: Orion "
              "Pirates and the official 2.5.5.2 patch on Windows, or under Wine or CrossOver, "
              "then import the folder it was installed in.");
}

TEST_CASE("the disc, an image of it and an installer are refused with what to do instead") {
    const Scratch s("sfcop-l2a-disc");
    s.write("disc/data1.cab", "invented cabinet\n");
    s.write("disc/setup.exe", "invented setup\n");
    s.write("disc.iso", "invented image\n");
    const std::string what = " is the game's disc, an image of it, an installer or an archive, "
                             "not an installed game. Install Starfleet Command: Orion Pirates and "
                             "the official 2.5.5.2 patch on Windows, or under Wine or CrossOver, "
                             "then import the folder it was installed in.";
    ControlledImportFiles files;
    const ImportReport disc = importGame(requestFor(s, "disc"), files);
    CHECK(disc.status == ImportStatus::kDiscOrInstaller);
    CHECK(disc.exitCode() == 3);
    CHECK(disc.message() == s.path("disc") + what);
    const ImportReport image = importGame(requestFor(s, "disc.iso"), files);
    CHECK(image.status == ImportStatus::kDiscOrInstaller);
    CHECK(image.message() == s.path("disc.iso") + what);
    CHECK(files.changes == 0);
}

TEST_CASE("missing, empty and truncated files are refused by name before anything is written") {
    const Scratch s("sfcop-l2a-damaged");
    writeGoodInstall(s);
    fs::remove(s.path("install/MetaAssets/shiplist.txt"));
    s.write("install/sfc.ini", "");
    s.write("install/Assets/Models/test.mod", "invented");
    ControlledImportFiles files;
    const ImportReport report = importGame(requestFor(s), files);
    CHECK(report.status == ImportStatus::kDamaged);
    CHECK(report.exitCode() == 3);
    CHECK(report.message() ==
          "The install at " + s.path("install") +
              " is damaged: these files are missing, empty or shorter than in the 2.5.5.2 "
              "release, so nothing was imported:\n"
              "  truncated: Assets/Models/test.mod (8 of 15 bytes)\n"
              "  missing: MetaAssets/shiplist.txt\n"
              "  empty: sfc.ini\n"
              "Copy these files from another install of 2.5.5.2, or reinstall and apply the "
              "official 2.5.5.2 patch, then import again.");
    CHECK(files.changes == 0);
    CHECK_FALSE(s.exists("data"));
}

TEST_CASE("an unchecked row needs the file there and whole by its own measure, not its hash") {
    CHECK(parseInstallManifest(std::string(kManifest) + "Assets/Models/a.bmp\t\t\n").ok());
    CHECK(parseInstallManifest(std::string(kManifest) + "Assets/Models/a.bmp\t\t\n")
              .entries.back()
              .checked() == false);
    CHECK(parseInstallManifest("StarFleetOP.exe\t\t\n").error ==
          "line 1: the row of StarFleetOP.exe must give its size and SHA-256");
    CHECK(parseInstallManifest(std::string(kManifest) + "a.bmp\t\tabc\n").error ==
          "line 8: the size '' is not a whole number");

    const std::string unchecked = std::string(kManifest) +
                                  "Assets/Models/sky.bmp\t\t\n"
                                  "Assets/Models/sea.bmp\t\t\n"
                                  "Assets/Models/odd.mod\t\t\n";
    // "BM", then the file's size as the header gives it, little-endian: 0x10 = 16 bytes.
    const std::string bmp16("BM\x10\x00\x00\x00invented!!", 16);
    const std::string bmpSays32("BM\x20\x00\x00\x00invented!!", 16);
    {
        const Scratch s("sfcop-l2a-unchecked");
        writeGoodInstall(s);
        s.write("install/Assets/Models/sky.bmp", bmp16);
        s.write("install/Assets/Models/sea.bmp", "not a bitmap at all\n");
        s.write("install/Assets/Models/odd.mod", "any model\n");
        ImportRequest request = requestFor(s);
        request.manifest = parseInstallManifest(unchecked);
        REQUIRE(request.manifest.ok());
        ControlledImportFiles files;
        const ImportReport report = importGame(request, files);
        REQUIRE(report.imported());
        CHECK(report.changed.empty());
        CHECK(report.filesCopied == 8);
        CHECK(s.read("data/game/Assets/Models/sky.bmp") == bmp16);
        // The record names the unchecked rows, so a run accepts them as the import did.
        const ImportRecord record = parseImportRecord(s.read("data/game/.sfcop-import.txt"));
        CHECK(record.changed.empty());
        CHECK(record.unchecked == std::vector<std::string>{"Assets/Models/sky.bmp",
                                                           "Assets/Models/sea.bmp",
                                                           "Assets/Models/odd.mod"});
    }
    {
        const Scratch s("sfcop-l2a-unchecked-bad");
        writeGoodInstall(s);
        s.write("install/Assets/Models/sky.bmp", bmpSays32);
        s.write("install/Assets/Models/sea.bmp", "");
        ImportRequest request = requestFor(s);
        request.manifest = parseInstallManifest(unchecked);
        REQUIRE(request.manifest.ok());
        ControlledImportFiles files;
        const ImportReport report = importGame(request, files);
        CHECK(report.status == ImportStatus::kDamaged);
        CHECK(report.message() ==
              "The install at " + s.path("install") +
                  " is damaged: these files are missing, empty or shorter than in the 2.5.5.2 "
                  "release, so nothing was imported:\n"
                  "  truncated: Assets/Models/sky.bmp (16 of 32 bytes)\n"
                  "  empty: Assets/Models/sea.bmp\n"
                  "  missing: Assets/Models/odd.mod\n"
                  "Copy these files from another install of 2.5.5.2, or reinstall and apply the "
                  "official 2.5.5.2 patch, then import again.");
        CHECK(files.changes == 0);
    }
}

TEST_CASE("a file with another hash is imported and listed; a script not in 2.5.5.2 is a mod") {
    const Scratch s("sfcop-l2a-changed");
    writeGoodInstall(s);
    s.write("install/Assets/Models/test.mod", "invented modeL\n");
    s.write("install/Assets/Scripts/MyMission.scr", "invented mod script\n");
    ControlledImportFiles files;
    const ImportReport report = importGame(requestFor(s), files);
    REQUIRE(report.imported());
    CHECK(report.changed == std::vector<std::string>{"Assets/Models/test.mod"});
    CHECK(report.modScripts == std::vector<std::string>{"Assets/Scripts/MyMission.scr"});
    CHECK(s.read("data/game/Assets/Models/test.mod") == "invented modeL\n");
    CHECK(s.read("data/game/Assets/Scripts/MyMission.scr") == "invented mod script\n");
    CHECK(report.message() ==
          "Imported 6 files (0.0 MB) from " + s.path("install") + " into " + s.path("data/game") +
              ".\n"
              "These files differ from the 2.5.5.2 release and were imported as they are:\n"
              "  Assets/Models/test.mod\n"
              "These mission scripts are not part of 2.5.5.2 and were imported as mod scripts:\n"
              "  mod script: Assets/Scripts/MyMission.scr");
    CHECK(s.read("data/game/.sfcop-import.txt").find("changed\tAssets/Models/test.mod\n"
                                                     "mod-script\tAssets/Scripts/MyMission.scr\n") !=
          std::string::npos);
}

TEST_CASE("the record of an import is read for its changed and unchecked files") {
    const ImportRecord record = parseImportRecord(
        "# What sfcop --import copied here. Not read by the game.\n"
        "manifest-files\t5\n"
        "source\t/games/Orion Pirates\n"
        "changed\tAssets/Models/test.mod\r\n"
        "mod-script\tAssets/Scripts/MyMission.scr\n"
        "unchecked\tAssets/Models/sky.bmp\n"
        "changed\t\n"
        "no tab here\n"
        "changed\tsfc.ini");
    CHECK(record.changed == std::vector<std::string>{"Assets/Models/test.mod", "sfc.ini"});
    CHECK(record.unchecked == std::vector<std::string>{"Assets/Models/sky.bmp"});
    CHECK(parseImportRecord("").changed.empty());
}

TEST_CASE("names are matched ignoring case and copied under the manifest's spelling") {
    const Scratch s("sfcop-l2a-case");
    writeGoodInstall(s);
    fs::rename(s.path("install/Assets"), s.path("install/assets.tmp"));
    fs::rename(s.path("install/assets.tmp"), s.path("install/assets"));
    fs::rename(s.path("install/assets/Models/test.mod"), s.path("install/assets/Models/TEST.MOD"));
    fs::rename(s.path("install/StarFleetOP.exe"), s.path("install/starfleetop.exe"));
    ControlledImportFiles files;
    const ImportReport report = importGame(requestFor(s), files);
    REQUIRE(report.imported());
    CHECK(s.read("data/game/Assets/Models/test.mod") == "invented model\n");
    CHECK(s.read("data/game/StarFleetOP.exe") == "invented exe 2.5.5.2\n");
}

TEST_CASE("too little free space is refused before anything is written") {
    const Scratch s("sfcop-l2a-space");
    writeGoodInstall(s);
    ControlledImportFiles files;
    files.free = 91; // 83 bytes plus a tenth (8) is 91: one more is needed
    ImportReport report = importGame(requestFor(s), files);
    REQUIRE(report.imported());
    REQUIRE(files.removeAll(s.path("data")));

    files.free = 90;
    files.changes = 0;
    report = importGame(requestFor(s), files);
    CHECK(report.status == ImportStatus::kNoSpace);
    CHECK(report.exitCode() == 1);
    CHECK(report.bytesNeeded == 91);
    CHECK(report.message() == "Importing needs 0.0 MB free at " + s.path("data") +
                                  ", and 0.0 MB are free. Free some space, then import again.");
    CHECK(files.changes == 0);
    CHECK_FALSE(s.exists("data"));
}

TEST_CASE("a cancelled import leaves no game folder, and an earlier one untouched") {
    const Scratch s("sfcop-l2a-cancel");
    writeGoodInstall(s);
    ControlledImportFiles files;
    // Cancelled once the first file is copied.
    bool stop = false;
    ImportCallbacks callbacks;
    callbacks.progress = [&stop](const ImportProgress &p) {
        stop = stop || (p.phase == ImportPhase::kCopying && p.bytesDone > 0);
    };
    callbacks.cancelled = [&stop] { return stop; };
    ImportReport report = importGame(requestFor(s), files, callbacks);
    CHECK(report.status == ImportStatus::kCancelled);
    CHECK(report.message() == "The import was cancelled; nothing was kept.");
    CHECK_FALSE(s.exists("data/game"));
    CHECK_FALSE(s.exists("data/game.partial"));

    s.write("data/game/earlier.txt", "earlier import\n");
    stop = false;
    report = importGame(requestFor(s), files, callbacks);
    CHECK(report.status == ImportStatus::kCancelled);
    CHECK(s.read("data/game/earlier.txt") == "earlier import\n");
    CHECK_FALSE(s.exists("data/game.partial"));
}

TEST_CASE("a second import replaces the first only after its own copy is verified") {
    const Scratch s("sfcop-l2a-again");
    writeGoodInstall(s);
    s.write("data/game/earlier.txt", "earlier import\n");
    s.write("data/game.partial/stale.txt", "from a run that was killed\n");
    ControlledImportFiles files;

    // A write that fails part way: the earlier import stays.
    files.failWrite = 3;
    ImportReport report = importGame(requestFor(s), files);
    CHECK(report.status == ImportStatus::kWriteFailed);
    CHECK(report.exitCode() == 1);
    CHECK(report.message() == "The import could not write " +
                                  s.path("data/game.partial/Assets/Scripts/Patrol.scr") +
                                  "; nothing was kept. Check that the folder is writable, then "
                                  "import again.");
    CHECK(s.read("data/game/earlier.txt") == "earlier import\n");
    CHECK_FALSE(s.exists("data/game.partial"));

    // A damaged source: the earlier import stays.
    files.failWrite = 0;
    s.write("install/sfc.ini", "");
    report = importGame(requestFor(s), files);
    CHECK(report.status == ImportStatus::kDamaged);
    CHECK(s.read("data/game/earlier.txt") == "earlier import\n");

    // A good one replaces it whole.
    s.write("install/sfc.ini", "[3D]\nwindowed=0\n");
    report = importGame(requestFor(s), files);
    REQUIRE(report.imported());
    CHECK_FALSE(s.exists("data/game/earlier.txt"));
    CHECK(s.read("data/game/sfc.ini") == "[3D]\nwindowed=0\n");
    CHECK_FALSE(s.exists("data/game.old"));
    CHECK_FALSE(s.exists("data/game.partial"));
}

TEST_CASE("an earlier import is kept when moving the new one in fails, or was cut short") {
    const Scratch s("sfcop-l2a-move");
    writeGoodInstall(s);
    s.write("data/game/earlier.txt", "earlier import\n");
    {
        // Rename 1 moves the earlier import aside; rename 2, the new one in, fails.
        ControlledImportFiles files;
        files.failRename = 2;
        const ImportReport report = importGame(requestFor(s), files);
        CHECK(report.status == ImportStatus::kWriteFailed);
        CHECK(report.path == s.path("data/game"));
        CHECK(s.read("data/game/earlier.txt") == "earlier import\n");
        CHECK_FALSE(s.exists("data/game.old"));
        CHECK_FALSE(s.exists("data/game.partial"));
    }
    {
        // A run stopped between the two renames left only game.old: the next import puts it
        // back first, so a failure of its own still leaves the earlier import.
        fs::rename(s.path("data/game"), s.path("data/game.old"));
        ControlledImportFiles files;
        files.failWrite = 3;
        const ImportReport report = importGame(requestFor(s), files);
        CHECK(report.status == ImportStatus::kWriteFailed);
        CHECK(s.read("data/game/earlier.txt") == "earlier import\n");
        CHECK_FALSE(s.exists("data/game.old"));
    }
    {
        // Putting game.old back fails: it is the only earlier import, so it is left as it is.
        fs::rename(s.path("data/game"), s.path("data/game.old"));
        ControlledImportFiles files;
        files.failRename = 1;
        const ImportReport report = importGame(requestFor(s), files);
        CHECK(report.status == ImportStatus::kWriteFailed);
        CHECK(report.path == s.path("data/game"));
        CHECK(report.earlierAt == s.path("data/game.old"));
        CHECK(s.read("data/game.old/earlier.txt") == "earlier import\n");
        CHECK_FALSE(s.exists("data/game"));
        CHECK_FALSE(s.exists("data/game.partial"));
        CHECK(files.renames == 1);
        fs::rename(s.path("data/game.old"), s.path("data/game"));
    }
    {
        // The new copy cannot be moved in, nor the earlier one back: the report says where the
        // earlier one is.
        ControlledImportFiles files;
        files.failRenamesFrom = 2;
        const ImportReport report = importGame(requestFor(s), files);
        CHECK(report.status == ImportStatus::kWriteFailed);
        CHECK(files.renames == 3);
        CHECK(report.earlierAt == s.path("data/game.old"));
        CHECK(s.read("data/game.old/earlier.txt") == "earlier import\n");
        CHECK(report.message() == "The import could not write " + s.path("data/game") +
                                      ". The earlier import is in " + s.path("data/game.old") +
                                      "; the next import puts it back first. Check that the "
                                      "folder is writable, then import again.");
    }
}

TEST_CASE("an unfinished or set-aside import is never taken as the install to import") {
    const Scratch s("sfcop-l2a-own");
    writeGoodInstall(s, "data/game");
    writeGoodInstall(s, "data/game.partial");
    writeGoodInstall(s, "data/game.old/inside");
    ControlledImportFiles files;
    const ImportReport report = importGame(requestFor(s, "data"), files);
    CHECK(report.status == ImportStatus::kNotFound);
    CHECK(files.changes == 0);
    CHECK(s.exists("data/game.old/inside/StarFleetOP.exe"));
}

TEST_CASE("the client's own copy is refused as the source, however it is reached") {
    const Scratch s("sfcop-l2a-own-source");
    writeGoodInstall(s, "data/game");
    writeGoodInstall(s, "data/game.partial");
    writeGoodInstall(s, "data/game.old");
    const bool links = makeLink(s.path("data/game.old"), s.path("to-old"), true) &&
                       makeLink(s.path("data/game.partial/Assets"), s.path("to-assets"), true);
    std::vector<std::string> sources = {"data/game",        "data/game.partial",
                                        "data/game.old",    "data/game/Assets",
                                        "data/game.old/Assets/Models", "data/GAME.OLD",
                                        "data/game.old/../game.partial"};
    if (links) {
        sources.push_back("to-old");
        sources.push_back("to-assets");
    }
    for (const std::string &source : sources) {
        CAPTURE(source);
        ControlledImportFiles files;
        const ImportReport report = importGame(requestFor(s, source), files);
        CHECK(report.status == ImportStatus::kOwnCopy);
        CHECK(report.exitCode() == 3);
        CHECK(files.changes == 0);
    }
    CHECK(s.read("data/game.old/Assets/Models/test.mod") == "invented model\n");
    CHECK(s.read("data/game.partial/Assets/Models/test.mod") == "invented model\n");
    ControlledImportFiles files;
    CHECK(importGame(requestFor(s, "data/game.old"), files).message() ==
          s.path("data/game.old") + " is the client's own copy of the game, or a folder of it, "
                                    "not an installed game. Choose the folder Starfleet Command: "
                                    "Orion Pirates is installed in.");
    // A folder beside them whose name only starts the same is not one of them.
    writeGoodInstall(s, "data/game.older");
    CHECK(importGame(requestFor(s, "data/game.older"), files).imported());
}

TEST_CASE("an install holding a link into the client's own copy is refused before any write") {
    const Scratch s("sfcop-l2a-own-link");
    writeGoodInstall(s);
    writeGoodInstall(s, "data/game.old");
    if (!makeLink(s.path("data/game.old/Assets"), s.path("install/Assets/Spare"), true)) {
        MESSAGE("links cannot be made here: the link cases are not tested");
        return;
    }
    ControlledImportFiles files;
    // A link to a folder that holds the copy is refused as well.
    fs::remove(s.path("install/Assets/Spare"));
    REQUIRE(makeLink(s.path("data"), s.path("install/Data"), true));
    const ImportReport above = importGame(requestFor(s), files);
    CHECK(above.status == ImportStatus::kOwnCopy);
    CHECK(above.ownCopyLink == "Data");
    CHECK(files.changes == 0);
    fs::remove(s.path("install/Data"));
    REQUIRE(makeLink(s.path("data/game.old/Assets"), s.path("install/Assets/Spare"), true));
    const ImportReport report = importGame(requestFor(s), files);
    CHECK(report.status == ImportStatus::kOwnCopy);
    CHECK(report.exitCode() == 3);
    CHECK(report.ownCopyLink == "Assets/Spare");
    CHECK(files.changes == 0);
    CHECK(s.read("data/game.old/Assets/Models/test.mod") == "invented model\n");
    CHECK(report.message() == "Assets/Spare in the install at " + s.path("install") +
                                  " links into the client's own copy of the game, or to a "
                                  "folder holding it, and the import replaces that copy, so "
                                  "nothing was imported. Remove the link, then import again.");
}

TEST_CASE("an install that holds the data folder is imported without reading the copy in it") {
    const Scratch s("sfcop-l2a-data-inside");
    writeGoodInstall(s);
    ImportRequest request = requestFor(s);
    request.gameDir = s.path("install/data/game");
    ControlledImportFiles files;
    REQUIRE(importGame(request, files).imported());
    // A link a modder left in the copy, to the data folder: read through, it would refuse the
    // install as a link holding the copy. The copy is passed over, so it is never read.
    if (!makeLink(s.path("install/data"), s.path("install/data/game/Assets/Up"), true)) {
        return;
    }
    const ImportReport again = importGame(request, files);
    CHECK(again.status == ImportStatus::kImported);
    CHECK(again.ownCopyLink.empty());
    CHECK(again.filesCopied == 5);
    CHECK(s.read("install/data/game/Assets/Models/test.mod") == "invented model\n");
}

TEST_CASE("a cancel while the executable is checked stops before the survey") {
    const Scratch s("sfcop-l2a-cancel-exe");
    writeGoodInstall(s);
    bool checking = false;
    bool surveyed = false;
    ImportCallbacks callbacks;
    callbacks.progress = [&](const ImportProgress &p) {
        checking = p.phase == ImportPhase::kCheckingGame;
        surveyed = surveyed || p.phase == ImportPhase::kCopying;
    };
    callbacks.cancelled = [&] { return checking; };
    ControlledImportFiles files;
    const ImportReport report = importGame(requestFor(s), files, callbacks);
    CHECK(report.status == ImportStatus::kCancelled);
    CHECK(report.exitCode() == 1);
    CHECK_FALSE(surveyed);
    CHECK(files.changes == 0);
}

TEST_CASE("files past the listing's limits are not called missing, and a loop is passed over") {
    const Scratch s("sfcop-l2a-deep");
    writeGoodInstall(s);
    // A link back to a folder above is passed over, so a file truly missing is called missing.
    if (!makeLink(s.path("install/Assets"), s.path("install/Assets/loop"), true)) {
        MESSAGE("links cannot be made here: the loop case is not tested");
        return;
    }
    ControlledImportFiles files;
    CHECK(importGame(requestFor(s), files).imported());
    fs::rename(s.path("install/Assets/Models/test.mod"), s.path("test.mod"));
    const ImportReport missing = importGame(requestFor(s), files);
    CHECK(missing.status == ImportStatus::kDamaged);
    REQUIRE(missing.damaged.size() == 1);
    CHECK(missing.damaged[0].path == "Assets/Models/test.mod");
    CHECK(missing.damaged[0].problem == "missing");
    fs::rename(s.path("test.mod"), s.path("install/Assets/Models/test.mod"));

    // A listed file below the limit cannot be found: the listing was cut, not the install.
    std::string deep;
    for (int i = 1; i <= 18; ++i) {
        deep += "d" + std::to_string(i) + "/";
    }
    deep += "x.txt";
    s.write("install/" + deep, "deep\n");
    ImportRequest request = requestFor(s);
    request.manifest = parseInstallManifest(std::string(kManifest) + deep + "\t\t\n");
    REQUIRE(request.manifest.ok());
    const ImportReport report = importGame(request, files);
    CHECK(report.status == ImportStatus::kTooManyFolders);
    CHECK(report.exitCode() == 3);
    CHECK(report.message() == "The install at " + s.path("install") +
                                  " holds more than 20000 folders, or folders more than 16 deep, "
                                  "so it was not all looked at and nothing was imported. Choose "
                                  "the folder Starfleet Command: Orion Pirates is installed in.");
}

TEST_CASE("an unusable manifest stops the import before the folder is looked at") {
    const Scratch s("sfcop-l2a-manifest");
    ImportRequest request = requestFor(s);
    request.manifest = parseInstallManifest("a.mod\t1\tzz\n");
    ControlledImportFiles files;
    const ImportReport report = importGame(request, files);
    CHECK(report.status == ImportStatus::kBadManifest);
    CHECK(report.exitCode() == 1);
    CHECK(report.message() == "The install manifest cannot be used (line 1: expected 64 hex "
                              "digits of SHA-256), so nothing can be imported. The client's files "
                              "are incomplete.");
}

TEST_CASE("the command imports, prints its progress and remembers the copy in install.txt") {
    const Scratch s("sfcop-l2a-command");
    writeGoodInstall(s);
    s.write("results/install_manifest.tsv", kManifest);
    fs::create_directories(s.path("data"));
    ControlledImportFiles importFiles;
    ImportCommand command;
    command.source = s.path("install");
    command.dataDir = s.path("data");
    command.gameDir = s.path("data/game");
    command.manifestPath = s.path("results/install_manifest.tsv");
    command.cancelled = [] { return false; };
    std::ostringstream out;
    std::ostringstream err;
    const int code = runImportCommand(command, hostFileSystem(), importFiles, out, err);
    CHECK(code == 0);
    CHECK(err.str().empty());
    CHECK(out.str() == "sfcop: import: looking for the game\n"
                       "sfcop: import: checking the game\n"
                       "sfcop: import: copying\n"
                       "sfcop: import: 20% (0.0 of 0.0 MB)\n"
                       "sfcop: import: 40% (0.0 of 0.0 MB)\n"
                       "sfcop: import: 60% (0.0 of 0.0 MB)\n"
                       "sfcop: import: 80% (0.0 of 0.0 MB)\n"
                       "sfcop: import: 100% (0.0 of 0.0 MB)\n"
                       "sfcop: import: finishing\n"
                       "sfcop: Imported 5 files (0.0 MB) from " +
                           s.path("install") + " into " + s.path("data/game") +
                           ".\n"
                           "sfcop: the next run starts from " +
                           s.path("data/game") + ".\n");
    CHECK(s.read("data/install.txt") == "game\n");

    command.source = s.path("nowhere");
    std::ostringstream out2;
    std::ostringstream err2;
    CHECK(runImportCommand(command, hostFileSystem(), importFiles, out2, err2) == 3);
    CHECK(err2.str() == "sfcop: No StarFleetOP.exe was found in " + s.path("nowhere") +
                            " or in the folders below it. Choose the folder Starfleet Command: "
                            "Orion Pirates is installed in.\n");

    command.manifestPath = s.path("results/none.tsv");
    std::ostringstream err3;
    CHECK(runImportCommand(command, hostFileSystem(), importFiles, out2, err3) == 1);
    CHECK(err3.str() == "sfcop: the install manifest " + s.path("results/none.tsv") +
                            " cannot be read, so nothing can be imported.\n");
}

TEST_CASE("an imported copy is verified in place, changed files and mod scripts listed") {
    const Scratch s("sfcop-verify-good");
    writeGoodInstall(s);
    ControlledImportFiles importer;
    REQUIRE(importGame(requestFor(s), importer).imported());
    s.write("data/game/Assets/Models/test.mod", "invented modeL\n");
    s.write("data/game/Assets/Scripts/MyMission.scr", "invented mod script\n");

    ControlledImportFiles files;
    std::vector<ImportPhase> phases;
    std::uint64_t lastDone = 0;
    std::uint64_t total = 0;
    ImportCallbacks callbacks;
    callbacks.progress = [&](const ImportProgress &p) {
        if (phases.empty() || phases.back() != p.phase) {
            phases.push_back(p.phase);
        }
        lastDone = p.bytesDone;
        total = p.bytesTotal;
    };
    const ImportReport report = verifyImportedGame(s.path("data/game"), requestFor(s).manifest,
                                                   files, callbacks);

    CHECK(report.status == ImportStatus::kVerified);
    CHECK(report.exitCode() == 0);
    CHECK(report.filesHashed == 5);
    CHECK(report.bytesHashed == 83);
    CHECK(report.changed == std::vector<std::string>{"Assets/Models/test.mod"});
    CHECK(report.modScripts == std::vector<std::string>{"Assets/Scripts/MyMission.scr"});
    CHECK(report.damaged.empty());
    CHECK(phases == std::vector<ImportPhase>{ImportPhase::kCheckingGame, ImportPhase::kVerifying});
    // The four checked rows other than the executable: 15 + 16 + 15 + 16 bytes.
    CHECK(total == 62);
    CHECK(lastDone == 62);
    CHECK(files.changes == 0);
    CHECK(report.message() ==
          "The imported game in " + s.path("data/game") +
              " is complete: 5 files (0.0 MB) checked against the 2.5.5.2 release.\n"
              "These files differ from the 2.5.5.2 release (imported so, or changed since):\n"
              "  Assets/Models/test.mod\n"
              "  mod script: Assets/Scripts/MyMission.scr");
}

TEST_CASE("verifying a copy names every missing, empty, short or unreadable file") {
    const Scratch s("sfcop-verify-damaged");
    writeGoodInstall(s);
    ControlledImportFiles importer;
    REQUIRE(importGame(requestFor(s), importer).imported());
    s.write("data/game/Assets/Scripts/Patrol.scr", "");
    fs::remove(s.path("data/game/MetaAssets/shiplist.txt"));
    s.write("data/game/sfc.ini", "[3D");

    ControlledImportFiles files;
    const InstallManifest manifest = requestFor(s).manifest;
    const ImportReport report = verifyImportedGame(s.path("data/game"), manifest, files);
    CHECK(report.status == ImportStatus::kDamaged);
    CHECK(report.exitCode() == 3);
    CHECK(files.changes == 0);
    CHECK(report.message() ==
          "The imported game in " + s.path("data/game") +
              " is damaged: these files are missing, empty, shorter than in the 2.5.5.2 release "
              "or unreadable:\n"
              "  empty: Assets/Scripts/Patrol.scr\n"
              "  missing: MetaAssets/shiplist.txt\n"
              "  truncated: sfc.ini (3 of 16 bytes)\n"
              "Import the game again from the installed game.");

    // A file that is there and long enough but cannot be read through is damaged too.
    const Scratch t("sfcop-verify-unreadable");
    writeGoodInstall(t);
    REQUIRE(importGame(requestFor(t), importer).imported());
    ControlledImportFiles refusing;
    refusing.unreadable = t.path("data/game/Assets/Models/test.mod");
    const ImportReport unreadable =
        verifyImportedGame(t.path("data/game"), manifest, refusing);
    CHECK(unreadable.status == ImportStatus::kDamaged);
    REQUIRE(unreadable.damaged.size() == 1);
    CHECK(unreadable.damaged[0].path == "Assets/Models/test.mod");
    CHECK(unreadable.damaged[0].problem == "unreadable");

    // A file shortened after the survey sized it reads short when hashed: damaged, not changed.
    ControlledImportFiles shortening;
    shortening.shortened = t.path("data/game/Assets/Models/test.mod");
    shortening.shortenedTo = 4;
    const ImportReport shortened = verifyImportedGame(t.path("data/game"), manifest, shortening);
    CHECK(shortened.status == ImportStatus::kDamaged);
    CHECK(shortened.changed.empty());
    REQUIRE(shortened.damaged.size() == 1);
    CHECK(shortened.damaged[0].path == "Assets/Models/test.mod");
    CHECK(shortened.damaged[0].problem == "truncated");
    CHECK(shortened.damaged[0].size == 4);
    CHECK(shortened.damaged[0].expectedSize == 15);
}

TEST_CASE("verifying with no copy, or a copy whose executable changed, says what to do") {
    const Scratch s("sfcop-verify-exe");
    const InstallManifest manifest = parseInstallManifest(kManifest);
    ControlledImportFiles files;
    const ImportReport none = verifyImportedGame(s.path("data/game"), manifest, files);
    CHECK(none.status == ImportStatus::kNotFound);
    CHECK(none.exitCode() == 3);
    CHECK(none.message() ==
          "There is no imported game in " + s.path("data/game") + ". Import the game first.");

    writeGoodInstall(s);
    REQUIRE(importGame(requestFor(s), files).imported());
    s.write("data/game/StarFleetOP.exe", "invented exe 2.5.5.3\n");
    const ImportReport modified = verifyImportedGame(s.path("data/game"), manifest, files);
    CHECK(modified.status == ImportStatus::kExeModified);
    CHECK(modified.exitCode() == 3);
    CHECK(modified.path == s.path("data/game/StarFleetOP.exe"));
    CHECK(modified.message() == s.path("data/game/StarFleetOP.exe") +
                                    " has been changed since it was imported. Import the game "
                                    "again from the installed game.");
}

TEST_CASE("a check follows links inside the copy and never refuses it as the client's own") {
    const Scratch s("sfcop-verify-links");
    writeGoodInstall(s);
    ControlledImportFiles files;
    REQUIRE(importGame(requestFor(s), files).imported());
    // A link to another folder of the copy, and one back to the data folder that holds it.
    if (!makeLink(s.path("data/game/Assets/Models"), s.path("data/game/Assets/Extra"), true) ||
        !makeLink(s.path("data"), s.path("data/game/Assets/Up"), true)) {
        return;
    }
    ControlledImportFiles checking;
    const ImportReport report =
        verifyImportedGame(s.path("data/game"), parseInstallManifest(kManifest), checking);
    CHECK(report.status == ImportStatus::kVerified);
    CHECK(report.exitCode() == 0);
    CHECK(report.ownCopyLink.empty());
    CHECK(report.filesHashed == 5);
    CHECK(checking.changes == 0);
}

TEST_CASE("a check of a copy too deep to list says so in a check's words") {
    const Scratch s("sfcop-verify-deep");
    writeGoodInstall(s);
    std::string deep;
    for (int i = 1; i <= 18; ++i) {
        deep += "d" + std::to_string(i) + "/";
    }
    deep += "x.txt";
    s.write("install/" + deep, "deep\n");
    ControlledImportFiles files;
    REQUIRE(importGame(requestFor(s), files).imported());
    s.write("data/game/" + deep, "deep\n");
    const InstallManifest manifest = parseInstallManifest(std::string(kManifest) + deep + "\t\t\n");
    REQUIRE(manifest.ok());
    const ImportReport report = verifyImportedGame(s.path("data/game"), manifest, files);
    CHECK(report.status == ImportStatus::kTooManyFolders);
    CHECK(report.exitCode() == 3);
    CHECK(report.message() == "The imported game in " + s.path("data/game") +
                                  " holds more than 20000 folders, or folders more than 16 deep, "
                                  "so it was not all looked at. Import the game again from the "
                                  "installed game.");
}

TEST_CASE("a check cancelled while the executable is checked hashes nothing else") {
    const Scratch s("sfcop-verify-cancel-exe");
    writeGoodInstall(s);
    ControlledImportFiles files;
    REQUIRE(importGame(requestFor(s), files).imported());
    bool checking = false;
    bool hashing = false;
    ImportCallbacks callbacks;
    callbacks.progress = [&](const ImportProgress &p) {
        checking = p.phase == ImportPhase::kCheckingGame;
        hashing = hashing || p.phase == ImportPhase::kVerifying;
    };
    callbacks.cancelled = [&] { return checking; };
    const ImportReport report =
        verifyImportedGame(s.path("data/game"), parseInstallManifest(kManifest), files, callbacks);
    CHECK(report.status == ImportStatus::kCancelled);
    CHECK(report.message() == "The check was cancelled.");
    CHECK_FALSE(hashing);
}

TEST_CASE("a cancelled check stops between pieces and leaves the copy as it was") {
    const Scratch s("sfcop-verify-cancel");
    writeGoodInstall(s);
    ControlledImportFiles files;
    REQUIRE(importGame(requestFor(s), files).imported());
    bool hashing = false;
    ImportCallbacks callbacks;
    callbacks.progress = [&](const ImportProgress &p) {
        hashing = p.phase == ImportPhase::kVerifying && p.bytesDone > 0;
    };
    callbacks.cancelled = [&] { return hashing; };
    const ImportReport report =
        verifyImportedGame(s.path("data/game"), parseInstallManifest(kManifest), files, callbacks);
    CHECK(report.status == ImportStatus::kCancelled);
    CHECK(report.exitCode() == 1);
    CHECK(report.message() == "The check was cancelled.");
    CHECK(s.read("data/game/Assets/Models/test.mod") == "invented model\n");

    const ImportReport bad =
        verifyImportedGame(s.path("data/game"), parseInstallManifest("a.mod\t1\tzz\n"), files);
    CHECK(bad.status == ImportStatus::kBadManifest);
    CHECK(bad.message() == "The install manifest cannot be used (line 1: expected 64 hex digits "
                           "of SHA-256), so the imported game cannot be checked. The client's "
                           "files are incomplete.");
}

TEST_CASE("the verify command prints its progress and report and writes nothing") {
    const Scratch s("sfcop-verify-command");
    writeGoodInstall(s);
    s.write("results/install_manifest.tsv", kManifest);
    ControlledImportFiles files;
    REQUIRE(importGame(requestFor(s), files).imported());
    const std::uint64_t changesAfterImport = files.changes;
    ImportCommand command;
    command.dataDir = s.path("data");
    command.gameDir = s.path("data/game");
    command.manifestPath = s.path("results/install_manifest.tsv");
    command.cancelled = [] { return false; };
    std::ostringstream out;
    std::ostringstream err;
    CHECK(runVerifyImportCommand(command, hostFileSystem(), files, out, err) == 0);
    CHECK(err.str().empty());
    // 15, 31, 46 and 62 of 62 bytes hashed.
    CHECK(out.str() == "sfcop: verify: checking the game\n"
                       "sfcop: verify: hashing\n"
                       "sfcop: verify: 20% (0.0 of 0.0 MB)\n"
                       "sfcop: verify: 50% (0.0 of 0.0 MB)\n"
                       "sfcop: verify: 70% (0.0 of 0.0 MB)\n"
                       "sfcop: verify: 100% (0.0 of 0.0 MB)\n"
                       "sfcop: The imported game in " +
                           s.path("data/game") +
                           " is complete: 5 files (0.0 MB) checked against the 2.5.5.2 "
                           "release.\n");
    CHECK(files.changes == changesAfterImport);

    command.gameDir = s.path("data/none");
    std::ostringstream out2;
    std::ostringstream err2;
    CHECK(runVerifyImportCommand(command, hostFileSystem(), files, out2, err2) == 3);
    CHECK(err2.str() == "sfcop: There is no imported game in " + s.path("data/none") +
                            ". Import the game first.\n");

    command.manifestPath = s.path("results/none.tsv");
    std::ostringstream err3;
    CHECK(runVerifyImportCommand(command, hostFileSystem(), files, out2, err3) == 1);
    CHECK(err3.str() == "sfcop: the install manifest " + s.path("results/none.tsv") +
                            " cannot be read, so the imported game cannot be checked.\n");
}
