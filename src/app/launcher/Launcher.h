// The launcher's screens as a state machine (DESIGN §5.1, §6.6): what a player with no game set up
// sees first, and what `--launcher` opens on demand. It holds no window, no thread and no file:
// the window (LauncherWindow.h) draws `view()` and reports what the player does, the jobs
// (LauncherJobs.h) run the import library on a worker thread and report back, and the
// environment writes install.txt and describes the game in use. So every screen, button and text
// is tested without a window (LauncherTest.cpp).
//
//   Welcome --choose a folder--> Working --imported--> Ready --Play--> the game starts
//      ^                            |                    |
//      |                            +--refused--> Result-+--Check game files--> Working --> Result
//      +--------------------Change game files------------+
//
// One job at a time: an import and a check never run together, and Play is not offered while
// either runs. Closing the window during a job cancels it and waits for it to stop.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "app/Import.h"

namespace sfcop::app::launcher {

enum class Screen {
    kWelcome, // choose the installed game's folder
    kWorking, // an import or a check is running
    kResult,  // what an import or a check ended with, when it is not simply Ready
    kReady,   // a game is set up: Play, Check game files, Change game files
    kClosing, // the window was closed during a job, which is stopping
};

// What a button does. The window draws `View::buttons` in order and reports the one pressed.
enum class Action {
    kChooseFolder,  // open the system's folder picker (the window does; it reports the choice)
    kUseTypedPath,  // import from the path field
    kCancel,        // stop the running job
    kPlay,          // start the game
    kCheck,         // check the imported copy
    kChangeGame,    // Ready -> Welcome
    kBackToGame,    // Welcome or Result -> Ready, when a game is set up
    kChooseAnother, // Result -> Welcome
    kTryAgain,      // the same import or step again
    kQuit,          // close the launcher without playing
};

struct Button {
    Action action;
    std::string label;
    // The button drawn as the screen's main action.
    bool primary = false;
};

// A folder the Welcome screen suggests.
struct Suggestion {
    std::string path;
    // The folder itself holds the game's executable, not only a folder above it.
    bool holdsGame = false;
};

// The game a run would start from now.
struct GameInUse {
    std::string dir;
    // The client's own imported copy; otherwise an install used in place (--install, or a path
    // install.txt names).
    bool imported = false;
    // The import record's: where the copy came from, and the files and bytes copied.
    std::string source;
    std::size_t files = 0;
    std::uint64_t bytes = 0;
    // When the copy was made, as the window shows it ("2026-11-12 14:03"); empty when unknown.
    std::string when;
};

enum class Tone { kPlain, kSuccess, kWarning, kError };

// A list a result screen shows below its message, scrollable.
struct ReportList {
    std::string heading;
    std::vector<std::string> lines;
    Tone tone = Tone::kWarning;
};

enum class FolderPicker {
    kAvailable,
    // The system has no folder picker (Linux without a portal or zenity): the path field and a
    // folder dropped on the window are the ways in, and Welcome says so.
    kUnavailable,
};

struct View {
    Screen screen = Screen::kWelcome;
    std::string title;
    // The screen's text, one paragraph per element; the report's message on a result screen.
    std::vector<std::string> paragraphs;
    Tone tone = Tone::kPlain;
    std::vector<ReportList> lists;
    std::vector<Button> buttons;

    // Welcome.
    std::vector<Suggestion> suggestions;
    bool pathField = false;
    bool acceptsDrop = false;
    // Why the folder picker cannot be used, when it cannot.
    std::string pickerNote;
    // A passing remark: the path typed was empty, an import or a check was cancelled.
    std::string note;

    // Working.
    std::string phase;
    std::string file;
    // 0 to 1 while bytes are counted; nothing for an indeterminate bar.
    std::optional<double> fraction;
    std::string amount; // "312.4 of 750.3 MB", "file 12 of 4215"
    bool cancelling = false;

    // Ready.
    std::optional<GameInUse> game;
};

// What the launcher needs from outside it. Every member is called on the window's thread.
struct Environment {
    // Writes install.txt to name the imported copy (DataDir.h, rememberInstall); false on failure.
    std::function<bool()> rememberImport;
    // The game a run would start from now, read again after each import and check.
    std::function<std::optional<GameInUse>()> gameInUse;
    // A folder chosen made absolute (Files.h, absolutePath); kept as chosen when empty.
    std::function<std::string(const std::string &)> absolute;
};

// Starts and stops the library's calls on a worker thread. The launcher asks for at most one at
// a time; the window delivers their progress and reports through onProgress and onFinished.
class Jobs {
public:
    virtual ~Jobs() = default;
    virtual void startImport(const ImportRequest &request) = 0;
    virtual void startCheck(const std::string &gameDir, const InstallManifest &manifest) = 0;
    // Asks the running job to stop; it still reports, usually kCancelled.
    virtual void cancel() = 0;
};

struct Setup {
    std::string dataDir;
    std::string gameDir;
    // install.txt's name, for the text when it cannot be written.
    std::string rememberedFile = "install.txt";
    InstallManifest manifest;
    std::vector<Suggestion> suggestions;
    FolderPicker picker = FolderPicker::kAvailable;
    // Open on Ready when a game is set up (`--launcher`), else Welcome.
    bool openOnReady = false;
};

enum class Outcome { kRunning, kPlay, kQuit };

class Launcher {
public:
    Launcher(Setup setup, Environment environment, Jobs &jobs);

    const View &view() const { return view_; }
    Outcome outcome() const { return outcome_; }
    bool busy() const { return busy_; }

    // A button of the current view (anything else is ignored). kUseTypedPath reads `typed`.
    void press(Action action, const std::string &typed = std::string());
    // A folder chosen in the picker or dropped on the window: imports it, from Welcome or Result.
    void choose(const std::string &folder);
    // The folder picker could not open (`error` from the system); Welcome then says so.
    void pickerFailed(const std::string &error);
    void onProgress(const ImportProgress &progress);
    void onFinished(const ImportReport &report);
    // The window's close button: quits, or cancels the running job and quits once it stops.
    void close();

private:
    enum class Job { kNone, kImport, kCheck };

    void startImport(const std::string &folder);
    void startCheck();
    void showWelcome(std::string note = std::string());
    void showReady(const std::optional<ImportReport> &imported = std::nullopt);
    void showWorking();
    void showImportResult(const ImportReport &report);
    void showCheckResult(const ImportReport &report);
    void showRememberFailed();
    void finishImport(const ImportReport &report);
    void refreshGame();
    bool hasButton(Action action) const;

    Setup setup_;
    Environment environment_;
    Jobs &jobs_;
    View view_;
    Outcome outcome_ = Outcome::kRunning;
    bool busy_ = false;
    Job job_ = Job::kNone;
    bool closing_ = false;
    // The folder of the last import started, for Try again.
    std::string lastFolder_;
    // Try again on a result screen writes install.txt again rather than importing.
    bool retryRemember_ = false;
    std::optional<ImportProgress> progress_;
    std::optional<GameInUse> game_;
};

} // namespace sfcop::app::launcher
