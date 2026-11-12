// `sfcop --import DIR`: the command line's thin caller of importGame (Import.h). It reads the
// install manifest, prints the phases and the copy's progress, prints the report, and on success
// writes install.txt so that later runs start from the copy with no --install.
#pragma once

#include <functional>
#include <iosfwd>
#include <string>

#include "app/Files.h"
#include "app/ImportFiles.h"

namespace sfcop::app {

struct ImportCommand {
    // The folder the player named.
    std::string source;
    // The data folder (absolute, already created) and the game folder in it (DataDir.h).
    std::string dataDir;
    std::string gameDir;
    // The install manifest (Import.h, installManifestBeside).
    std::string manifestPath;
    // Polled during the import; when empty, Ctrl-C (SIGINT) cancels it.
    std::function<bool()> cancelled;
    // Seconds on a steady clock, for the time the import took; nothing printed when empty.
    std::function<double()> seconds;
};

// 0 imported, 3 the folder refused, 1 the import could not finish (ImportReport::exitCode).
int runImportCommand(const ImportCommand &command, FileSystem &files, ImportFiles &importFiles,
                     std::ostream &out, std::ostream &err);

// `sfcop --verify-import`: verifyImportedGame over command.gameDir (`source` is not used), its
// progress and report printed; nothing is written. 0 the copy is whole, 3 it is missing or
// damaged, 1 the check could not finish (ImportReport::exitCode).
int runVerifyImportCommand(const ImportCommand &command, FileSystem &files,
                           ImportFiles &importFiles, std::ostream &out, std::ostream &err);

} // namespace sfcop::app
