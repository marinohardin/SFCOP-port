// The launcher's screens without a window: a fake Jobs records what the launcher asks for, and
// the tests hand it the progress and reports the import library would send. Every expected
// screen, button and text is written out.
#include "app/launcher/Launcher.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace sfcop::app::launcher {
namespace {

struct FakeJobs final : Jobs {
    std::vector<ImportRequest> imports;
    std::vector<std::string> checks;
    int cancels = 0;

    void startImport(const ImportRequest &request) override { imports.push_back(request); }
    void startCheck(const std::string &gameDir, const InstallManifest &) override {
        checks.push_back(gameDir);
    }
    void cancel() override { ++cancels; }
};

const char *const kData = "/home/TestCaptain/.local/share/SFCOP";
const char *const kGame = "/home/TestCaptain/.local/share/SFCOP/game";

GameInUse importedGame() {
    GameInUse game;
    game.dir = kGame;
    game.imported = true;
    game.source = "/games/Orion Pirates";
    game.files = 4215;
    game.bytes = 750312448;
    game.when = "2026-11-12 14:03";
    return game;
}

// A launcher over FakeJobs whose environment is under the test's control.
struct Rig {
    FakeJobs jobs;
    std::optional<GameInUse> game;
    bool rememberWorks = true;
    int remembered = 0;
    Setup setup;

    Rig() {
        setup.dataDir = kData;
        setup.gameDir = kGame;
        setup.suggestions = {{"/home/TestCaptain/.wine/drive_c", false},
                             {"/games/Orion Pirates", true}};
    }

    Launcher make() {
        Environment environment;
        environment.rememberImport = [this] {
            ++remembered;
            if (rememberWorks) {
                game = importedGame();
            }
            return rememberWorks;
        };
        environment.gameInUse = [this] { return game; };
        environment.absolute = [](const std::string &path) {
            return path.front() == '/' ? path : "/home/TestCaptain/" + path;
        };
        return Launcher(setup, environment, jobs);
    }
};

std::vector<Action> actions(const View &view) {
    std::vector<Action> result;
    for (const Button &shown : view.buttons) {
        result.push_back(shown.action);
    }
    return result;
}

std::vector<std::string> labels(const View &view) {
    std::vector<std::string> result;
    for (const Button &shown : view.buttons) {
        result.push_back(shown.label);
    }
    return result;
}

std::string primary(const View &view) {
    for (const Button &shown : view.buttons) {
        if (shown.primary) {
            return shown.label;
        }
    }
    return std::string();
}

ImportReport importReport(ImportStatus status) {
    ImportReport report;
    report.status = status;
    report.source = "/games/Orion Pirates";
    report.installDir = "/games/Orion Pirates";
    report.gameDir = kGame;
    return report;
}

ImportReport checkReport(ImportStatus status) {
    ImportReport report;
    report.status = status;
    report.verify = true;
    report.installDir = kGame;
    report.gameDir = kGame;
    return report;
}

// Starts an import of /games/Orion Pirates from Welcome.
void startImport(Launcher &launcher) {
    launcher.choose("/games/Orion Pirates");
    REQUIRE(launcher.view().screen == Screen::kWorking);
}

const std::string kWelcomeText =
    "SFCOP plays the game from your own copy. It needs Starfleet Command: Orion Pirates installed "
    "and patched to version 2.5.5.2; the disc alone is not enough. Choose the folder the game is "
    "installed in, or a folder above it such as a Wine, CrossOver or Proton prefix: the game "
    "files are found below it and copied into SFCOP's own folder.";

TEST_CASE("a first run opens Welcome with the picker, the path field, a drop target and the "
          "suggestions") {
    Rig rig;
    Launcher launcher = rig.make();
    const View &view = launcher.view();
    CHECK(view.screen == Screen::kWelcome);
    CHECK(view.title == "Set up Starfleet Command: Orion Pirates");
    CHECK(view.paragraphs == std::vector<std::string>{kWelcomeText});
    CHECK(labels(view) ==
          std::vector<std::string>{"Choose folder...", "Import this folder", "Quit"});
    CHECK(primary(view) == "Choose folder...");
    CHECK(view.pathField);
    CHECK(view.acceptsDrop);
    CHECK(view.pickerNote.empty());
    REQUIRE(view.suggestions.size() == 2);
    CHECK(view.suggestions[0].path == "/home/TestCaptain/.wine/drive_c");
    CHECK_FALSE(view.suggestions[0].holdsGame);
    CHECK(view.suggestions[1].path == "/games/Orion Pirates");
    CHECK(view.suggestions[1].holdsGame);
    CHECK(launcher.outcome() == Outcome::kRunning);
}

TEST_CASE("without a folder picker Welcome says so and the path field is the main way in") {
    Rig rig;
    rig.setup.picker = FolderPicker::kUnavailable;
    Launcher launcher = rig.make();
    const View &view = launcher.view();
    CHECK(view.pickerNote ==
          "This system has no folder picker (on Linux it needs the desktop portal or zenity). "
          "Type the folder's path below, or drop the folder on this window.");
    CHECK(actions(view) == std::vector<Action>{Action::kUseTypedPath, Action::kQuit});
    CHECK(primary(view) == "Import this folder");
    CHECK(view.acceptsDrop);
    // A dropped folder still imports.
    launcher.choose("/games/Orion Pirates");
    CHECK(launcher.view().screen == Screen::kWorking);
}

TEST_CASE("a picker that fails to open is reported and the launcher carries on without it") {
    Rig rig;
    Launcher launcher = rig.make();
    launcher.pickerFailed("no portal");
    const View &view = launcher.view();
    CHECK(view.screen == Screen::kWelcome);
    CHECK(view.pickerNote ==
          "The folder picker could not be opened. Type the folder's path below, or drop the "
          "folder on this window. The system said: no portal");
    CHECK(actions(view) == std::vector<Action>{Action::kUseTypedPath, Action::kQuit});
}

TEST_CASE("a typed path is trimmed, made absolute and imported; an empty one asks for a path") {
    Rig rig;
    Launcher launcher = rig.make();
    launcher.press(Action::kUseTypedPath, "   ");
    CHECK(launcher.view().screen == Screen::kWelcome);
    CHECK(launcher.view().note ==
          "Type the path of the folder the game is installed in, then press Import this folder.");
    CHECK(rig.jobs.imports.empty());

    launcher.press(Action::kUseTypedPath, " 'Games/Orion Pirates' \n");
    REQUIRE(rig.jobs.imports.size() == 1);
    CHECK(rig.jobs.imports[0].source == "/home/TestCaptain/Games/Orion Pirates");
    CHECK(rig.jobs.imports[0].gameDir == kGame);
}

TEST_CASE("Working shows the phase, the file and a byte bar only while bytes are counted") {
    Rig rig;
    Launcher launcher = rig.make();
    startImport(launcher);
    const View &view = launcher.view();
    CHECK(view.title == "Importing the game");
    CHECK(view.phase == "Looking for the game in the folder you chose");
    CHECK_FALSE(view.fraction.has_value());
    CHECK(labels(view) == std::vector<std::string>{"Cancel"});
    CHECK(launcher.busy());

    ImportProgress progress;
    progress.phase = ImportPhase::kCheckingGame;
    progress.bytesDone = 5; // not counted in this phase
    progress.bytesTotal = 10;
    launcher.onProgress(progress);
    CHECK(launcher.view().phase == "Checking the game's program and its files");
    CHECK_FALSE(launcher.view().fraction.has_value());
    CHECK(launcher.view().amount.empty());

    progress.phase = ImportPhase::kCopying;
    progress.fileIndex = 11;
    progress.fileCount = 4215;
    progress.path = "Assets/Models/fed-ca.mod";
    progress.bytesDone = 187500000;
    progress.bytesTotal = 750000000;
    launcher.onProgress(progress);
    CHECK(launcher.view().phase == "Copying the game files");
    CHECK(launcher.view().file == "Assets/Models/fed-ca.mod");
    REQUIRE(launcher.view().fraction.has_value());
    CHECK(*launcher.view().fraction == doctest::Approx(0.25));
    CHECK(launcher.view().amount == "187.5 of 750.0 MB, file 12 of 4215");

    progress.phase = ImportPhase::kFinishing;
    progress.path.clear();
    launcher.onProgress(progress);
    CHECK(launcher.view().phase == "Finishing");
    CHECK_FALSE(launcher.view().fraction.has_value());
}

TEST_CASE("an import that succeeds writes install.txt and shows Ready with the report") {
    Rig rig;
    Launcher launcher = rig.make();
    startImport(launcher);
    ImportReport report = importReport(ImportStatus::kImported);
    report.filesCopied = 4215;
    report.bytesCopied = 750312448;
    launcher.onFinished(report);
    CHECK(rig.remembered == 1);
    const View &view = launcher.view();
    CHECK(view.screen == Screen::kReady);
    CHECK(view.title == "Ready to play");
    CHECK(view.tone == Tone::kSuccess);
    CHECK(view.paragraphs ==
          std::vector<std::string>{
              "The game is imported from /games/Orion Pirates on 2026-11-12 14:03 (4215 files, "
              "750.3 MB).",
              "Imported 4215 files (750.3 MB) from /games/Orion Pirates into " +
                  std::string(kGame) + "."});
    CHECK(labels(view) == std::vector<std::string>{"Play", "Check game files",
                                                   "Change game files", "Quit"});
    CHECK(primary(view) == "Play");
    CHECK_FALSE(launcher.busy());
    launcher.press(Action::kPlay);
    CHECK(launcher.outcome() == Outcome::kPlay);
}

TEST_CASE("an import with changed files or mod scripts shows Ready as a warning") {
    Rig rig;
    Launcher launcher = rig.make();
    startImport(launcher);
    ImportReport report = importReport(ImportStatus::kImported);
    report.filesCopied = 6;
    report.changed = {"Assets/Models/test.mod"};
    report.modScripts = {"Assets/Scripts/MyMission.scr"};
    launcher.onFinished(report);
    CHECK(launcher.view().screen == Screen::kReady);
    CHECK(launcher.view().tone == Tone::kWarning);
    REQUIRE(launcher.view().paragraphs.size() == 2);
    CHECK(launcher.view().paragraphs[1] ==
          "Imported 6 files (0.0 MB) from /games/Orion Pirates into " + std::string(kGame) +
              ".\n"
              "These files differ from the 2.5.5.2 release and were imported as they are:\n"
              "  Assets/Models/test.mod\n"
              "These mission scripts are not part of 2.5.5.2 and were imported as mod scripts:\n"
              "  mod script: Assets/Scripts/MyMission.scr");
}

TEST_CASE("when install.txt cannot be written the player is told and can try again") {
    Rig rig;
    rig.rememberWorks = false;
    Launcher launcher = rig.make();
    startImport(launcher);
    ImportReport report = importReport(ImportStatus::kImported);
    report.filesCopied = 4215;
    launcher.onFinished(report);
    const View &view = launcher.view();
    CHECK(view.screen == Screen::kResult);
    CHECK(view.tone == Tone::kError);
    CHECK(view.title == "The game was imported, but cannot be remembered");
    CHECK(view.paragraphs ==
          std::vector<std::string>{
              "The game was imported into " + std::string(kGame) +
              ", but install.txt could not be written in " + kData +
              ", so the next start would not find it. Check that the folder is writable, then "
              "press Try again, or start SFCOP with --install " + kGame + "."});
    CHECK(labels(view) == std::vector<std::string>{"Try again", "Quit"});
    CHECK_FALSE(view.acceptsDrop);

    rig.rememberWorks = true;
    launcher.press(Action::kTryAgain);
    CHECK(rig.remembered == 2);
    CHECK(rig.jobs.imports.size() == 1); // install.txt again, not a second import
    CHECK(launcher.view().screen == Screen::kReady);
}

struct ImportCase {
    ImportStatus status;
    std::string title;
    std::vector<std::string> buttons;
};

TEST_CASE("every refused or failed import shows its message as an error with what to do next") {
    const std::vector<ImportCase> cases = {
        {ImportStatus::kNotFound, "No game was found in that folder",
         {"Choose another folder", "Quit"}},
        {ImportStatus::kDiscOrInstaller, "That is the disc or an installer, not an installed game",
         {"Choose another folder", "Quit"}},
        {ImportStatus::kVersion2500, "The game needs the official 2.5.5.2 patch",
         {"Try again", "Choose another folder", "Quit"}},
        {ImportStatus::kExeModified, "The game's program has been modified",
         {"Try again", "Choose another folder", "Quit"}},
        {ImportStatus::kNotTheGame, "That is not Starfleet Command: Orion Pirates 2.5.5.2",
         {"Choose another folder", "Quit"}},
        {ImportStatus::kDamaged, "Some of the game's files are missing or damaged",
         {"Try again", "Choose another folder", "Quit"}},
        {ImportStatus::kNoSpace, "There is not enough free space",
         {"Try again", "Choose another folder", "Quit"}},
        {ImportStatus::kWriteFailed, "The game files could not be written",
         {"Try again", "Choose another folder", "Quit"}},
        {ImportStatus::kBadManifest, "This copy of SFCOP is incomplete", {"Quit"}},
        {ImportStatus::kOwnCopy, "That folder is SFCOP's own copy of the game",
         {"Choose another folder", "Quit"}},
        {ImportStatus::kTooManyFolders, "That folder is too large to search",
         {"Choose another folder", "Quit"}},
    };
    for (const ImportCase &one : cases) {
        CAPTURE(one.title);
        Rig rig;
        Launcher launcher = rig.make();
        startImport(launcher);
        const ImportReport report = importReport(one.status);
        launcher.onFinished(report);
        const View &view = launcher.view();
        CHECK(view.screen == Screen::kResult);
        CHECK(view.tone == Tone::kError);
        CHECK(view.title == one.title);
        CHECK(view.paragraphs == std::vector<std::string>{report.message()});
        CHECK(labels(view) == one.buttons);
        CHECK(primary(view) == one.buttons.front());
        CHECK(rig.remembered == 0);
        CHECK(view.acceptsDrop == (one.status != ImportStatus::kBadManifest));
    }
}

TEST_CASE("the result texts name the file at fault and the action to take") {
    Rig rig;
    Launcher launcher = rig.make();
    startImport(launcher);
    ImportReport report = importReport(ImportStatus::kDamaged);
    report.damaged = {{"Assets/Models/fed-ca.mod", "truncated", 100, 2048},
                      {"Assets/Sounds/hit.wav", "missing", 0, 512}};
    launcher.onFinished(report);
    CHECK(launcher.view().paragraphs ==
          std::vector<std::string>{
              "The install at /games/Orion Pirates is damaged: these files are missing, empty or "
              "shorter than in the 2.5.5.2 release, so nothing was imported:\n"
              "  truncated: Assets/Models/fed-ca.mod (100 of 2048 bytes)\n"
              "  missing: Assets/Sounds/hit.wav\n"
              "Copy these files from another install of 2.5.5.2, or reinstall and apply the "
              "official 2.5.5.2 patch, then import again."});

    Rig space;
    Launcher full = space.make();
    startImport(full);
    ImportReport noSpace = importReport(ImportStatus::kNoSpace);
    noSpace.path = kData;
    noSpace.bytesNeeded = 825000000;
    noSpace.bytesFree = 300000000;
    full.onFinished(noSpace);
    CHECK(full.view().paragraphs ==
          std::vector<std::string>{"Importing needs 825.0 MB free at " + std::string(kData) +
                                   ", and 300.0 MB are free. Free some space, then import "
                                   "again."});
}

TEST_CASE("Try again imports the same folder; Choose another folder goes back to Welcome") {
    Rig rig;
    Launcher launcher = rig.make();
    startImport(launcher);
    launcher.onFinished(importReport(ImportStatus::kVersion2500));
    launcher.press(Action::kTryAgain);
    REQUIRE(rig.jobs.imports.size() == 2);
    CHECK(rig.jobs.imports[1].source == "/games/Orion Pirates");
    CHECK(launcher.view().screen == Screen::kWorking);
    launcher.onFinished(importReport(ImportStatus::kNotFound));
    launcher.press(Action::kChooseAnother);
    CHECK(launcher.view().screen == Screen::kWelcome);
}

TEST_CASE("a folder dropped on a result screen imports it") {
    Rig rig;
    Launcher launcher = rig.make();
    startImport(launcher);
    launcher.onFinished(importReport(ImportStatus::kNotFound));
    launcher.choose("/games/Orion Pirates 2");
    REQUIRE(rig.jobs.imports.size() == 2);
    CHECK(rig.jobs.imports[1].source == "/games/Orion Pirates 2");
}

TEST_CASE("an own-copy refusal caused by a link offers to try again once the link is removed") {
    Rig rig;
    Launcher launcher = rig.make();
    startImport(launcher);
    ImportReport report = importReport(ImportStatus::kOwnCopy);
    report.ownCopyLink = "Assets/link";
    launcher.onFinished(report);
    CHECK(labels(launcher.view()) ==
          std::vector<std::string>{"Choose another folder", "Try again", "Quit"});
}

TEST_CASE("cancelling an import in each phase returns quietly to Welcome") {
    for (const ImportPhase phase : {ImportPhase::kSearching, ImportPhase::kCheckingGame,
                                    ImportPhase::kCopying, ImportPhase::kFinishing}) {
        CAPTURE(static_cast<int>(phase));
        Rig rig;
        Launcher launcher = rig.make();
        startImport(launcher);
        ImportProgress progress;
        progress.phase = phase;
        launcher.onProgress(progress);
        launcher.press(Action::kCancel);
        CHECK(rig.jobs.cancels == 1);
        CHECK(launcher.view().cancelling);
        CHECK(launcher.view().note == "Stopping...");
        CHECK(launcher.view().buttons.empty());
        launcher.press(Action::kCancel); // no second cancel, and no button to press
        CHECK(rig.jobs.cancels == 1);
        CHECK(launcher.busy());
        launcher.onFinished(importReport(ImportStatus::kCancelled));
        CHECK(launcher.view().screen == Screen::kWelcome);
        CHECK(launcher.view().tone == Tone::kPlain);
        CHECK(launcher.view().note == "The import was cancelled; nothing was kept.");
        CHECK(rig.remembered == 0);
        CHECK_FALSE(launcher.busy());
    }
}

TEST_CASE("an import that finishes although Cancel was pressed is still kept") {
    Rig rig;
    Launcher launcher = rig.make();
    startImport(launcher);
    launcher.press(Action::kCancel);
    ImportReport report = importReport(ImportStatus::kImported);
    report.filesCopied = 1;
    launcher.onFinished(report); // the cancel came after the last piece
    CHECK(rig.remembered == 1);
    CHECK(launcher.view().screen == Screen::kReady);
}

TEST_CASE("closing the window during an import cancels it and quits once it has stopped") {
    Rig rig;
    Launcher launcher = rig.make();
    startImport(launcher);
    launcher.close();
    CHECK(rig.jobs.cancels == 1);
    CHECK(launcher.view().screen == Screen::kClosing);
    CHECK(launcher.view().title == "Closing");
    CHECK(launcher.view().buttons.empty());
    CHECK(launcher.outcome() == Outcome::kRunning);
    launcher.close(); // a second close waits as well
    CHECK(rig.jobs.cancels == 1);
    launcher.onProgress(ImportProgress{});
    CHECK(launcher.view().screen == Screen::kClosing);
    launcher.onFinished(importReport(ImportStatus::kCancelled));
    CHECK(launcher.outcome() == Outcome::kQuit);
    CHECK(rig.remembered == 0);
}

TEST_CASE("closing the window during an import that completes anyway does not remember it") {
    Rig rig;
    Launcher launcher = rig.make();
    startImport(launcher);
    launcher.close();
    ImportReport report = importReport(ImportStatus::kImported);
    report.filesCopied = 1;
    launcher.onFinished(report);
    CHECK(launcher.outcome() == Outcome::kQuit);
    CHECK(rig.remembered == 0);
}

TEST_CASE("closing the window while nothing runs quits at once") {
    Rig rig;
    Launcher launcher = rig.make();
    launcher.close();
    CHECK(launcher.outcome() == Outcome::kQuit);
    CHECK(rig.jobs.cancels == 0);
}

TEST_CASE("--launcher with a game opens Ready with Play, Check game files and Change game files") {
    Rig rig;
    rig.game = importedGame();
    rig.setup.openOnReady = true;
    Launcher launcher = rig.make();
    const View &view = launcher.view();
    CHECK(view.screen == Screen::kReady);
    CHECK(view.tone == Tone::kPlain);
    CHECK(view.paragraphs ==
          std::vector<std::string>{"The game is imported from /games/Orion Pirates on "
                                   "2026-11-12 14:03 (4215 files, 750.3 MB)."});
    CHECK(labels(view) == std::vector<std::string>{"Play", "Check game files",
                                                   "Change game files", "Quit"});
    REQUIRE(view.game.has_value());
    CHECK(view.game->dir == kGame);
}

TEST_CASE("--launcher with a game used in place offers no check of a copy") {
    Rig rig;
    GameInUse game;
    game.dir = "/games/Orion Pirates";
    rig.game = game;
    rig.setup.openOnReady = true;
    Launcher launcher = rig.make();
    CHECK(launcher.view().paragraphs ==
          std::vector<std::string>{
              "The game installed in /games/Orion Pirates is used as it is."});
    CHECK(labels(launcher.view()) ==
          std::vector<std::string>{"Play", "Change game files", "Quit"});
}

TEST_CASE("--launcher with no game opens Welcome") {
    Rig rig;
    rig.setup.openOnReady = true;
    Launcher launcher = rig.make();
    CHECK(launcher.view().screen == Screen::kWelcome);
}

TEST_CASE("Change game files goes to Welcome, which keeps a way back to the game") {
    Rig rig;
    rig.game = importedGame();
    rig.setup.openOnReady = true;
    Launcher launcher = rig.make();
    launcher.press(Action::kChangeGame);
    const View &view = launcher.view();
    CHECK(view.screen == Screen::kWelcome);
    CHECK(view.paragraphs ==
          std::vector<std::string>{kWelcomeText, "The game you have now stays in use until a new "
                                                 "import succeeds."});
    CHECK(labels(view) == std::vector<std::string>{"Choose folder...", "Import this folder",
                                                   "Back", "Quit"});
    launcher.press(Action::kBackToGame);
    CHECK(launcher.view().screen == Screen::kReady);
}

TEST_CASE("a second import over an earlier one keeps the earlier game until it succeeds") {
    Rig rig;
    rig.game = importedGame();
    rig.setup.openOnReady = true;
    Launcher launcher = rig.make();
    launcher.press(Action::kChangeGame);
    launcher.choose("/games/Orion Pirates 2");
    REQUIRE(rig.jobs.imports.size() == 1);
    CHECK(rig.jobs.imports[0].gameDir == kGame);
    CHECK(labels(launcher.view()) == std::vector<std::string>{"Cancel"}); // no Play meanwhile

    // Refused: the earlier game is still offered.
    launcher.onFinished(importReport(ImportStatus::kNotTheGame));
    CHECK(labels(launcher.view()) ==
          std::vector<std::string>{"Choose another folder", "Back", "Quit"});
    launcher.press(Action::kBackToGame);
    CHECK(launcher.view().screen == Screen::kReady);
    CHECK(launcher.view().game->source == "/games/Orion Pirates");

    // Succeeds: Ready shows the new game, read again.
    launcher.press(Action::kChangeGame);
    launcher.choose("/games/Orion Pirates 2");
    ImportReport report = importReport(ImportStatus::kImported);
    report.installDir = "/games/Orion Pirates 2";
    report.filesCopied = 4215;
    rig.rememberWorks = true;
    launcher.onFinished(report);
    CHECK(rig.remembered == 1);
    CHECK(launcher.view().screen == Screen::kReady);
}

TEST_CASE("Check game files runs the check and shows a complete copy as a success") {
    Rig rig;
    rig.game = importedGame();
    rig.setup.openOnReady = true;
    Launcher launcher = rig.make();
    launcher.press(Action::kCheck);
    CHECK(rig.jobs.checks == std::vector<std::string>{kGame});
    CHECK(launcher.view().screen == Screen::kWorking);
    CHECK(launcher.view().title == "Checking the game files");
    CHECK(launcher.view().phase == "Checking the game's program and its files");

    ImportProgress progress;
    progress.phase = ImportPhase::kVerifying;
    progress.fileIndex = 0;
    progress.fileCount = 4066;
    progress.bytesDone = 1048576;
    progress.bytesTotal = 736000000;
    launcher.onProgress(progress);
    CHECK(launcher.view().phase == "Checking each game file");
    CHECK(launcher.view().amount == "1.0 of 736.0 MB, file 1 of 4066");

    ImportReport report = checkReport(ImportStatus::kVerified);
    report.filesHashed = 4067;
    report.bytesHashed = 741200000;
    launcher.onFinished(report);
    const View &view = launcher.view();
    CHECK(view.screen == Screen::kResult);
    CHECK(view.title == "The game files are complete");
    CHECK(view.tone == Tone::kSuccess);
    CHECK(view.paragraphs ==
          std::vector<std::string>{"The imported game in " + std::string(kGame) +
                                   " is complete: 4067 files (741.2 MB) checked against the "
                                   "2.5.5.2 release."});
    CHECK(view.lists.empty());
    CHECK(labels(view) == std::vector<std::string>{"Back", "Quit"});
    CHECK(primary(view) == "Back");
    launcher.press(Action::kBackToGame);
    CHECK(launcher.view().screen == Screen::kReady);
}

TEST_CASE("a complete copy with changed files shows them prominently as a warning") {
    Rig rig;
    rig.game = importedGame();
    rig.setup.openOnReady = true;
    Launcher launcher = rig.make();
    launcher.press(Action::kCheck);
    ImportReport report = checkReport(ImportStatus::kVerified);
    report.changed = {"Assets/Models/test.mod", "sfc.ini"};
    launcher.onFinished(report);
    const View &view = launcher.view();
    CHECK(view.tone == Tone::kWarning);
    REQUIRE(view.lists.size() == 1);
    CHECK(view.lists[0].heading ==
          "These files differ from the 2.5.5.2 release. The game is complete, but it is not the "
          "stock 2.5.5.2 game:");
    CHECK(view.lists[0].lines == std::vector<std::string>{"Assets/Models/test.mod", "sfc.ini"});
    CHECK(view.lists[0].tone == Tone::kWarning);
}

struct CheckCase {
    ImportStatus status;
    std::string title;
    std::vector<std::string> buttons;
};

TEST_CASE("every failed check shows its message as an error with a way to import again") {
    const std::vector<CheckCase> cases = {
        {ImportStatus::kNotFound, "There is no imported game to check",
         {"Import the game", "Quit"}},
        {ImportStatus::kVersion2500, "The imported game's program has changed",
         {"Import the game again", "Back", "Quit"}},
        {ImportStatus::kExeModified, "The imported game's program has changed",
         {"Import the game again", "Back", "Quit"}},
        {ImportStatus::kNotTheGame, "The imported game's program has changed",
         {"Import the game again", "Back", "Quit"}},
        {ImportStatus::kDamaged, "Some of the imported game's files are missing or damaged",
         {"Import the game again", "Back", "Quit"}},
        {ImportStatus::kTooManyFolders, "The imported game could not be checked",
         {"Import the game again", "Back", "Quit"}},
        {ImportStatus::kBadManifest, "This copy of SFCOP is incomplete", {"Quit"}},
    };
    for (const CheckCase &one : cases) {
        CAPTURE(one.title);
        Rig rig;
        rig.game = importedGame();
        rig.setup.openOnReady = true;
        Launcher launcher = rig.make();
        launcher.press(Action::kCheck);
        if (one.status == ImportStatus::kNotFound) {
            rig.game.reset(); // the copy is gone, so there is no game to go back to
        }
        const ImportReport report = checkReport(one.status);
        launcher.onFinished(report);
        const View &view = launcher.view();
        CHECK(view.screen == Screen::kResult);
        CHECK(view.tone == Tone::kError);
        CHECK(view.title == one.title);
        CHECK(view.paragraphs == std::vector<std::string>{report.message()});
        CHECK(labels(view) == one.buttons);
        if (one.buttons.front() != "Quit") {
            launcher.press(Action::kChooseAnother);
            CHECK(launcher.view().screen == Screen::kWelcome);
        }
    }
}

TEST_CASE("cancelling a check in each phase returns quietly to Ready") {
    for (const ImportPhase phase : {ImportPhase::kCheckingGame, ImportPhase::kVerifying}) {
        CAPTURE(static_cast<int>(phase));
        Rig rig;
        rig.game = importedGame();
        rig.setup.openOnReady = true;
        Launcher launcher = rig.make();
        launcher.press(Action::kCheck);
        ImportProgress progress;
        progress.phase = phase;
        launcher.onProgress(progress);
        launcher.press(Action::kCancel);
        CHECK(rig.jobs.cancels == 1);
        launcher.onFinished(checkReport(ImportStatus::kCancelled));
        CHECK(launcher.view().screen == Screen::kReady);
        CHECK(launcher.view().note == "The check was cancelled; nothing was changed.");
    }
}

TEST_CASE("closing the window during a check cancels it and quits once it has stopped") {
    Rig rig;
    rig.game = importedGame();
    rig.setup.openOnReady = true;
    Launcher launcher = rig.make();
    launcher.press(Action::kCheck);
    launcher.close();
    CHECK(rig.jobs.cancels == 1);
    CHECK(launcher.view().screen == Screen::kClosing);
    launcher.onFinished(checkReport(ImportStatus::kCancelled));
    CHECK(launcher.outcome() == Outcome::kQuit);
}

TEST_CASE("only one job runs at a time: a choice or a button while one runs is ignored") {
    Rig rig;
    rig.game = importedGame();
    rig.setup.openOnReady = true;
    Launcher launcher = rig.make();
    launcher.press(Action::kCheck);
    launcher.choose("/games/Orion Pirates");
    launcher.press(Action::kPlay);
    launcher.press(Action::kCheck);
    launcher.press(Action::kUseTypedPath, "/games/Orion Pirates");
    CHECK(rig.jobs.imports.empty());
    CHECK(rig.jobs.checks.size() == 1);
    CHECK(launcher.outcome() == Outcome::kRunning);
    // A report from no job is ignored.
    launcher.onFinished(checkReport(ImportStatus::kVerified));
    Rig idle;
    Launcher quiet = idle.make();
    quiet.onFinished(importReport(ImportStatus::kImported));
    CHECK(quiet.view().screen == Screen::kWelcome);
    CHECK(idle.remembered == 0);
}

TEST_CASE("Quit ends the launcher without playing") {
    Rig rig;
    Launcher launcher = rig.make();
    launcher.press(Action::kQuit);
    CHECK(launcher.outcome() == Outcome::kQuit);
    launcher.choose("/games/Orion Pirates");
    CHECK(rig.jobs.imports.empty());
}

} // namespace
} // namespace sfcop::app::launcher
