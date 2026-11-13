// Every string the launcher shows, in one place (English only for now). The import library's own
// report text (ImportReport::message) is shown as it is and is not repeated here.
#pragma once

#include <cstdint>
#include <string>

#include "app/Import.h"

namespace sfcop::app::launcher::text {

// The window.
inline constexpr const char kWindowTitle[] = "SFCOP";

// Welcome.
inline constexpr const char kWelcomeTitle[] = "Set up Starfleet Command: Orion Pirates";
inline constexpr const char kWelcomeNeeds[] =
    "SFCOP plays the game from your own copy. It needs Starfleet Command: Orion Pirates "
    "installed and patched to version 2.5.5.2; the disc alone is not enough. Choose the folder "
    "the game is installed in, or a folder above it such as a Wine, CrossOver or Proton prefix: "
    "the game files are found below it and copied into SFCOP's own folder.";
inline constexpr const char kWelcomeChangeNote[] =
    "The game you have now stays in use until a new import succeeds.";
inline constexpr const char kChooseFolder[] = "Choose folder...";
inline constexpr const char kPathFieldLabel[] = "Or type the folder's path";
inline constexpr const char kUsePath[] = "Import this folder";
inline constexpr const char kDropHint[] = "You can also drop the folder on this window.";
inline constexpr const char kSuggestionsHeading[] = "Places to look";
inline constexpr const char kSuggestionHoldsGame[] = "the game is here";
inline constexpr const char kSuggestionLookBelow[] = "look in the folders below";
inline constexpr const char kEmptyPath[] =
    "Type the path of the folder the game is installed in, then press Import this folder.";
inline constexpr const char kPickerUnavailable[] =
    "This system has no folder picker (on Linux it needs the desktop portal or zenity). Type "
    "the folder's path below, or drop the folder on this window.";
// The system's error is added after it.
inline constexpr const char kPickerFailed[] =
    "The folder picker could not be opened. Type the folder's path below, or drop the folder on "
    "this window. The system said: ";
inline constexpr const char kCancelledImport[] = "The import was cancelled; nothing was kept.";

// Working.
inline constexpr const char kImportingTitle[] = "Importing the game";
inline constexpr const char kCheckingTitle[] = "Checking the game files";
inline constexpr const char kCancel[] = "Cancel";
inline constexpr const char kCancelling[] = "Stopping...";
inline constexpr const char kWorkingNote[] =
    "This copies about 750 MB and takes from a few seconds to a few minutes.";
inline constexpr const char kClosingTitle[] = "Closing";
inline constexpr const char kClosingNote[] =
    "Stopping the work in progress. The window closes when it has stopped; nothing half-done is "
    "kept.";

// Ready.
inline constexpr const char kReadyTitle[] = "Ready to play";
inline constexpr const char kPlay[] = "Play";
inline constexpr const char kCheck[] = "Check game files";
inline constexpr const char kChangeGame[] = "Change game files";
inline constexpr const char kCancelledCheck[] = "The check was cancelled; nothing was changed.";

// Result screens.
inline constexpr const char kChooseAnother[] = "Choose another folder";
inline constexpr const char kTryAgain[] = "Try again";
inline constexpr const char kImportAgain[] = "Import the game again";
inline constexpr const char kImportGame[] = "Import the game";
inline constexpr const char kBackToGame[] = "Back";
inline constexpr const char kQuit[] = "Quit";
inline constexpr const char kChangedHeading[] =
    "These files differ from the 2.5.5.2 release. The game is complete, but it is not the stock "
    "2.5.5.2 game:";
inline constexpr const char kRememberFailedTitle[] = "The game was imported, but cannot be remembered";

// The phase of a running import or check, in words.
std::string phase(ImportPhase phase);
// The title of the screen an import's report ends on, by status.
std::string importTitle(ImportStatus status);
// The title of the screen a check's report ends on, by status.
std::string checkTitle(ImportStatus status);
// "312.4 of 750.3 MB".
std::string megabytesOf(std::uint64_t done, std::uint64_t total);
// "file 12 of 4215" (1-based).
std::string fileOf(std::size_t index, std::size_t count);
// The paragraphs of Ready.
std::string readyImported(const std::string &source, const std::string &when, std::size_t files,
                          std::uint64_t bytes);
std::string readyInPlace(const std::string &dir);
// When install.txt cannot be written after an import (as `sfcop --import` says it).
std::string rememberFailed(const std::string &file, const std::string &dataDir,
                           const std::string &gameDir);

} // namespace sfcop::app::launcher::text
