#include "app/ImportCommand.h"

#include <csignal>
#include <cstdio>
#include <optional>
#include <ostream>

#include "app/DataDir.h"
#include "app/ExitCodes.h"
#include "app/Import.h"

namespace sfcop::app {

namespace {

volatile std::sig_atomic_t gInterrupted = 0;

// The first Ctrl-C asks the import to stop; the handler then steps aside, so a second one ends
// the process even when a read has stalled (a network drive) and the import cannot notice.
extern "C" void onInterrupt(int) {
    gInterrupted = 1;
    std::signal(SIGINT, SIG_DFL);
}

// Ctrl-C cancels the import while this lives, rather than killing the process mid-copy.
class InterruptCancels {
public:
    InterruptCancels() : previous_(std::signal(SIGINT, onInterrupt)) { gInterrupted = 0; }
    ~InterruptCancels() { std::signal(SIGINT, previous_ == SIG_ERR ? SIG_DFL : previous_); }
    InterruptCancels(const InterruptCancels &) = delete;
    InterruptCancels &operator=(const InterruptCancels &) = delete;

private:
    void (*previous_)(int);
};

const char *phaseText(ImportPhase phase) {
    switch (phase) {
    case ImportPhase::kSearching:
        return "looking for the game";
    case ImportPhase::kCheckingGame:
        return "checking the game";
    case ImportPhase::kCopying:
        return "copying";
    case ImportPhase::kFinishing:
        return "finishing";
    }
    return "";
}

} // namespace

int runImportCommand(const ImportCommand &command, FileSystem &files, ImportFiles &importFiles,
                     std::ostream &out, std::ostream &err) {
    const std::optional<std::string> text = files.readText(command.manifestPath);
    if (!text) {
        err << "sfcop: the install manifest " << command.manifestPath
            << " cannot be read, so nothing can be imported.\n";
        return kExitFailure;
    }
    ImportRequest request;
    request.source = files.absolutePath(command.source);
    request.gameDir = command.gameDir;
    request.manifest = parseInstallManifest(*text);

    const double started = command.seconds ? command.seconds() : 0.0;
    std::optional<ImportPhase> lastPhase;
    int lastTenth = -1;
    ImportCallbacks callbacks;
    callbacks.progress = [&out, &lastPhase, &lastTenth](const ImportProgress &progress) {
        if (lastPhase != progress.phase) {
            lastPhase = progress.phase;
            out << "sfcop: import: " << phaseText(progress.phase) << "\n" << std::flush;
        }
        if (progress.phase != ImportPhase::kCopying || progress.bytesDone == 0) {
            return;
        }
        const int tenth = progress.bytesTotal == 0
                              ? 10
                              : static_cast<int>(progress.bytesDone * 10 / progress.bytesTotal);
        if (tenth != lastTenth) {
            lastTenth = tenth;
            char line[96];
            std::snprintf(line, sizeof line, "sfcop: import: %d%% (%.1f of %.1f MB)\n",
                          tenth * 10, static_cast<double>(progress.bytesDone) / 1e6,
                          static_cast<double>(progress.bytesTotal) / 1e6);
            out << line << std::flush;
        }
    };
    std::optional<InterruptCancels> interrupt;
    if (command.cancelled) {
        callbacks.cancelled = command.cancelled;
    } else {
        interrupt.emplace();
        callbacks.cancelled = [] { return gInterrupted != 0; };
    }

    const ImportReport report = importGame(request, importFiles, callbacks);
    interrupt.reset();
    if (!report.imported()) {
        err << "sfcop: " << report.message() << "\n";
        return report.exitCode();
    }
    out << "sfcop: " << report.message() << "\n";
    if (command.seconds) {
        char line[96];
        std::snprintf(line, sizeof line, "sfcop: import took %.1f s (%zu of %zu files cloned)\n",
                      command.seconds() - started, report.filesCloned, report.filesCopied);
        out << line;
    }
    if (!rememberInstall(command.dataDir, kImportedGame, files)) {
        err << "sfcop: the game was imported, but " << kRememberedInstallFile << " could not be "
            << "written in " << command.dataDir << "; start sfcop with --install "
            << command.gameDir << ".\n";
        return kExitFailure;
    }
    out << "sfcop: the next run starts from " << command.gameDir << ".\n";
    return kExitSuccess;
}

} // namespace sfcop::app
