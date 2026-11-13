#include "app/launcher/LauncherText.h"

#include <cstdio>

namespace sfcop::app::launcher::text {

std::string phase(ImportPhase phase) {
    switch (phase) {
    case ImportPhase::kSearching:
        return "Looking for the game in the folder you chose";
    case ImportPhase::kCheckingGame:
        return "Checking the game's program and its files";
    case ImportPhase::kCopying:
        return "Copying the game files";
    case ImportPhase::kFinishing:
        return "Finishing";
    case ImportPhase::kVerifying:
        return "Checking each game file";
    }
    return std::string();
}

std::string importTitle(ImportStatus status) {
    switch (status) {
    case ImportStatus::kImported:
        return kReadyTitle;
    case ImportStatus::kNotFound:
        return "No game was found in that folder";
    case ImportStatus::kDiscOrInstaller:
        return "That is the disc or an installer, not an installed game";
    case ImportStatus::kVersion2500:
        return "The game needs the official 2.5.5.2 patch";
    case ImportStatus::kExeModified:
        return "The game's program has been modified";
    case ImportStatus::kNotTheGame:
        return "That is not Starfleet Command: Orion Pirates 2.5.5.2";
    case ImportStatus::kDamaged:
        return "Some of the game's files are missing or damaged";
    case ImportStatus::kNoSpace:
        return "There is not enough free space";
    case ImportStatus::kCancelled:
        return "The import was cancelled";
    case ImportStatus::kWriteFailed:
        return "The game files could not be written";
    case ImportStatus::kBadManifest:
        return "This copy of SFCOP is incomplete";
    case ImportStatus::kOwnCopy:
        return "That folder is SFCOP's own copy of the game";
    case ImportStatus::kTooManyFolders:
        return "That folder is too large to search";
    case ImportStatus::kVerified:
        return "The game files are complete";
    }
    return std::string();
}

std::string checkTitle(ImportStatus status) {
    switch (status) {
    case ImportStatus::kVerified:
        return "The game files are complete";
    case ImportStatus::kNotFound:
        return "There is no imported game to check";
    case ImportStatus::kVersion2500:
    case ImportStatus::kExeModified:
    case ImportStatus::kNotTheGame:
        return "The imported game's program has changed";
    case ImportStatus::kDamaged:
        return "Some of the imported game's files are missing or damaged";
    case ImportStatus::kTooManyFolders:
        return "The imported game could not be checked";
    case ImportStatus::kCancelled:
        return "The check was cancelled";
    case ImportStatus::kBadManifest:
        return "This copy of SFCOP is incomplete";
    default:
        return importTitle(status);
    }
}

std::string megabytesOf(std::uint64_t done, std::uint64_t total) {
    char line[64];
    std::snprintf(line, sizeof line, "%.1f of %.1f MB", static_cast<double>(done) / 1e6,
                  static_cast<double>(total) / 1e6);
    return line;
}

std::string fileOf(std::size_t index, std::size_t count) {
    return "file " + std::to_string(index + 1) + " of " + std::to_string(count);
}

std::string readyImported(const std::string &source, const std::string &when, std::size_t files,
                          std::uint64_t bytes) {
    std::string text = "The game is imported";
    if (!source.empty()) {
        text += " from " + source;
    }
    if (!when.empty()) {
        text += " on " + when;
    }
    if (files > 0) {
        char amount[64];
        std::snprintf(amount, sizeof amount, " (%zu files, %.1f MB)", files,
                      static_cast<double>(bytes) / 1e6);
        text += amount;
    }
    return text + ".";
}

std::string readyInPlace(const std::string &dir) {
    return "The game installed in " + dir + " is used as it is.";
}

std::string rememberFailed(const std::string &file, const std::string &dataDir,
                           const std::string &gameDir) {
    return "The game was imported into " + gameDir + ", but " + file + " could not be written in " +
           dataDir + ", so the next start would not find it. Check that the folder is writable, "
           "then press Try again, or start SFCOP with --install " + gameDir + ".";
}

} // namespace sfcop::app::launcher::text
