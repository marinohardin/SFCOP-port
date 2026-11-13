#include "app/launcher/Launcher.h"

#include <utility>

#include "app/launcher/LauncherText.h"

namespace sfcop::app::launcher {

namespace {

// Spaces around a typed or pasted path, and the quotes a terminal or a file manager adds.
std::string trimmedPath(const std::string &text) {
    const char *space = " \t\r\n";
    const std::size_t first = text.find_first_not_of(space);
    if (first == std::string::npos) {
        return std::string();
    }
    std::string value = text.substr(first, text.find_last_not_of(space) - first + 1);
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
        value.back() == value.front()) {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

Button button(Action action, const char *label, bool primary = false) {
    return Button{action, label, primary};
}

} // namespace

Launcher::Launcher(Setup setup, Environment environment, Jobs &jobs)
    : setup_(std::move(setup)), environment_(std::move(environment)), jobs_(jobs) {
    refreshGame();
    if (setup_.openOnReady && game_) {
        showReady();
    } else {
        showWelcome();
    }
}

void Launcher::refreshGame() {
    game_ = environment_.gameInUse ? environment_.gameInUse() : std::nullopt;
}

bool Launcher::hasButton(Action action) const {
    for (const Button &shown : view_.buttons) {
        if (shown.action == action) {
            return true;
        }
    }
    return false;
}

void Launcher::press(Action action, const std::string &typed) {
    if (outcome_ != Outcome::kRunning || !hasButton(action)) {
        return;
    }
    switch (action) {
    case Action::kChooseFolder:
        return; // the window opens the picker and reports through choose() or pickerFailed()
    case Action::kUseTypedPath: {
        const std::string folder = trimmedPath(typed);
        if (folder.empty()) {
            showWelcome(text::kEmptyPath);
            return;
        }
        startImport(folder);
        return;
    }
    case Action::kCancel:
        if (busy_ && !view_.cancelling) {
            jobs_.cancel();
            view_.cancelling = true;
            view_.buttons.clear();
            view_.note = text::kCancelling;
        }
        return;
    case Action::kPlay:
        outcome_ = Outcome::kPlay;
        return;
    case Action::kCheck:
        startCheck();
        return;
    case Action::kChangeGame:
    case Action::kChooseAnother:
        showWelcome();
        return;
    case Action::kBackToGame:
        showReady();
        return;
    case Action::kTryAgain:
        if (retryRemember_) {
            ImportReport imported;
            imported.status = ImportStatus::kImported;
            finishImport(imported);
        } else if (!lastFolder_.empty()) {
            startImport(lastFolder_);
        }
        return;
    case Action::kQuit:
        outcome_ = Outcome::kQuit;
        return;
    }
}

void Launcher::choose(const std::string &folder) {
    const bool open = view_.screen == Screen::kWelcome || view_.screen == Screen::kResult;
    if (outcome_ != Outcome::kRunning || busy_ || !open || !view_.acceptsDrop) {
        return;
    }
    const std::string trimmed = trimmedPath(folder);
    if (!trimmed.empty()) {
        startImport(trimmed);
    }
}

void Launcher::pickerFailed(const std::string &error) {
    setup_.picker = FolderPicker::kUnavailable;
    if (view_.screen == Screen::kWelcome) {
        showWelcome();
        view_.pickerNote = std::string(text::kPickerFailed) + error;
    }
}

void Launcher::startImport(const std::string &folder) {
    lastFolder_ = folder;
    retryRemember_ = false;
    ImportRequest request;
    request.source = environment_.absolute ? environment_.absolute(folder) : folder;
    request.gameDir = setup_.gameDir;
    request.manifest = setup_.manifest;
    job_ = Job::kImport;
    busy_ = true;
    progress_.reset();
    showWorking();
    jobs_.startImport(request);
}

void Launcher::startCheck() {
    job_ = Job::kCheck;
    busy_ = true;
    progress_.reset();
    showWorking();
    jobs_.startCheck(setup_.gameDir, setup_.manifest);
}

void Launcher::onProgress(const ImportProgress &progress) {
    if (!busy_) {
        return;
    }
    progress_ = progress;
    if (view_.screen != Screen::kWorking) {
        return;
    }
    view_.phase = text::phase(progress.phase);
    view_.file = progress.path;
    const bool counted =
        progress.phase == ImportPhase::kCopying || progress.phase == ImportPhase::kVerifying;
    if (counted && progress.bytesTotal > 0) {
        view_.fraction = static_cast<double>(progress.bytesDone) /
                         static_cast<double>(progress.bytesTotal);
        view_.amount = text::megabytesOf(progress.bytesDone, progress.bytesTotal);
        if (progress.fileCount > 0) {
            view_.amount += ", " + text::fileOf(progress.fileIndex, progress.fileCount);
        }
    } else {
        view_.fraction.reset();
        view_.amount.clear();
    }
}

void Launcher::onFinished(const ImportReport &report) {
    if (!busy_) {
        return;
    }
    const Job finished = job_;
    busy_ = false;
    job_ = Job::kNone;
    progress_.reset();
    if (closing_) {
        outcome_ = Outcome::kQuit;
        return;
    }
    if (finished == Job::kImport) {
        if (report.status == ImportStatus::kCancelled) {
            showWelcome(text::kCancelledImport);
        } else if (report.status == ImportStatus::kImported) {
            finishImport(report);
        } else {
            showImportResult(report);
        }
        return;
    }
    refreshGame();
    if (report.status == ImportStatus::kCancelled) {
        showReady();
        view_.note = text::kCancelledCheck;
        return;
    }
    showCheckResult(report);
}

void Launcher::close() {
    if (outcome_ != Outcome::kRunning) {
        return;
    }
    if (!busy_) {
        outcome_ = Outcome::kQuit;
        return;
    }
    if (!closing_) {
        closing_ = true;
        jobs_.cancel();
    }
    view_ = View{};
    view_.screen = Screen::kClosing;
    view_.title = text::kClosingTitle;
    view_.paragraphs = {text::kClosingNote};
}

// After kImported: install.txt names the copy, as `sfcop --import` leaves it, then Ready.
void Launcher::finishImport(const ImportReport &report) {
    if (!environment_.rememberImport || !environment_.rememberImport()) {
        retryRemember_ = true;
        showRememberFailed();
        return;
    }
    retryRemember_ = false;
    refreshGame();
    if (report.message().empty() || report.filesCopied == 0) {
        showReady();
    } else {
        showReady(report);
    }
}

void Launcher::showWelcome(std::string note) {
    view_ = View{};
    view_.screen = Screen::kWelcome;
    view_.title = text::kWelcomeTitle;
    view_.paragraphs = {text::kWelcomeNeeds};
    if (game_) {
        view_.paragraphs.push_back(text::kWelcomeChangeNote);
    }
    view_.suggestions = setup_.suggestions;
    view_.pathField = true;
    view_.acceptsDrop = true;
    view_.note = std::move(note);
    if (setup_.picker == FolderPicker::kAvailable) {
        view_.buttons.push_back(button(Action::kChooseFolder, text::kChooseFolder, true));
    } else {
        view_.pickerNote = text::kPickerUnavailable;
    }
    view_.buttons.push_back(button(Action::kUseTypedPath, text::kUsePath,
                                   setup_.picker != FolderPicker::kAvailable));
    if (game_) {
        view_.buttons.push_back(button(Action::kBackToGame, text::kBackToGame));
    }
    view_.buttons.push_back(button(Action::kQuit, text::kQuit));
}

void Launcher::showWorking() {
    view_ = View{};
    view_.screen = Screen::kWorking;
    view_.title = job_ == Job::kImport ? text::kImportingTitle : text::kCheckingTitle;
    if (job_ == Job::kImport) {
        view_.paragraphs = {text::kWorkingNote};
    }
    view_.phase = text::phase(job_ == Job::kImport ? ImportPhase::kSearching
                                                   : ImportPhase::kCheckingGame);
    view_.buttons.push_back(button(Action::kCancel, text::kCancel, true));
}

void Launcher::showReady(const std::optional<ImportReport> &imported) {
    if (!game_) {
        showWelcome();
        return;
    }
    view_ = View{};
    view_.screen = Screen::kReady;
    view_.title = text::kReadyTitle;
    view_.game = game_;
    view_.paragraphs = {game_->imported ? text::readyImported(game_->source, game_->when,
                                                              game_->files, game_->bytes)
                                        : text::readyInPlace(game_->dir)};
    if (imported) {
        view_.paragraphs.push_back(imported->message());
        const bool warn = !imported->changed.empty() || !imported->modScripts.empty();
        view_.tone = warn ? Tone::kWarning : Tone::kSuccess;
    }
    view_.buttons.push_back(button(Action::kPlay, text::kPlay, true));
    if (game_->imported) {
        view_.buttons.push_back(button(Action::kCheck, text::kCheck));
    }
    view_.buttons.push_back(button(Action::kChangeGame, text::kChangeGame));
    view_.buttons.push_back(button(Action::kQuit, text::kQuit));
}

void Launcher::showImportResult(const ImportReport &report) {
    view_ = View{};
    view_.screen = Screen::kResult;
    view_.title = text::importTitle(report.status);
    view_.paragraphs = {report.message()};
    view_.tone = Tone::kError;
    view_.acceptsDrop = report.status != ImportStatus::kBadManifest;
    switch (report.status) {
    case ImportStatus::kVersion2500:
    case ImportStatus::kExeModified:
    case ImportStatus::kDamaged:
    case ImportStatus::kNoSpace:
    case ImportStatus::kWriteFailed:
        // The player fixes the install, the space or the folder, then imports the same folder.
        view_.buttons.push_back(button(Action::kTryAgain, text::kTryAgain, true));
        view_.buttons.push_back(button(Action::kChooseAnother, text::kChooseAnother));
        break;
    case ImportStatus::kOwnCopy:
        view_.buttons.push_back(button(Action::kChooseAnother, text::kChooseAnother, true));
        if (!report.ownCopyLink.empty()) {
            view_.buttons.push_back(button(Action::kTryAgain, text::kTryAgain));
        }
        break;
    case ImportStatus::kBadManifest:
        break; // the client is broken: nothing the player chooses helps
    default:
        view_.buttons.push_back(button(Action::kChooseAnother, text::kChooseAnother, true));
        break;
    }
    if (game_ && report.status != ImportStatus::kBadManifest) {
        view_.buttons.push_back(button(Action::kBackToGame, text::kBackToGame));
    }
    view_.buttons.push_back(
        button(Action::kQuit, text::kQuit, report.status == ImportStatus::kBadManifest));
}

void Launcher::showCheckResult(const ImportReport &report) {
    view_ = View{};
    view_.screen = Screen::kResult;
    view_.title = text::checkTitle(report.status);
    view_.paragraphs = {report.message()};
    if (report.status == ImportStatus::kVerified) {
        view_.tone = report.changed.empty() ? Tone::kSuccess : Tone::kWarning;
        if (!report.changed.empty()) {
            view_.lists.push_back(ReportList{text::kChangedHeading, report.changed, Tone::kWarning});
        }
        if (game_) {
            view_.buttons.push_back(button(Action::kBackToGame, text::kBackToGame, true));
        }
        view_.buttons.push_back(button(Action::kQuit, text::kQuit, !game_));
        return;
    }
    view_.tone = Tone::kError;
    if (report.status == ImportStatus::kBadManifest) {
        view_.buttons.push_back(button(Action::kQuit, text::kQuit, true));
        return;
    }
    const bool none = report.status == ImportStatus::kNotFound;
    view_.buttons.push_back(
        button(Action::kChooseAnother, none ? text::kImportGame : text::kImportAgain, true));
    if (game_) {
        view_.buttons.push_back(button(Action::kBackToGame, text::kBackToGame));
    }
    view_.buttons.push_back(button(Action::kQuit, text::kQuit));
}

void Launcher::showRememberFailed() {
    view_ = View{};
    view_.screen = Screen::kResult;
    view_.title = text::kRememberFailedTitle;
    view_.paragraphs = {
        text::rememberFailed(setup_.rememberedFile, setup_.dataDir, setup_.gameDir)};
    view_.tone = Tone::kError;
    view_.buttons.push_back(button(Action::kTryAgain, text::kTryAgain, true));
    view_.buttons.push_back(button(Action::kQuit, text::kQuit));
}

} // namespace sfcop::app::launcher
