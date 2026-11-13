// Importing the player's installed game into the client's own folder (DESIGN §5.1, §6.6).
//
// The player names a folder: an installed, patched 2.5.5.2 game, or a folder above one such as a
// Wine, CrossOver or Proton prefix, searched a bounded number of levels down. The import checks
// the game's executable, refuses a damaged install by name before writing anything, checks the
// free space, then copies the import set (the files the install manifest lists, plus any mission
// script that is not in it) into `<game>.partial`, hashing each clone as read back and each copy
// from the bytes read for it (a copy is not read back), and only after every file is hashed
// renames it to the game folder, replacing an earlier import. A cancelled or failed import leaves
// no game folder of its own and the earlier one untouched.
//
// The install manifest (analysis/results/install_manifest.tsv, tools/install_manifest.py) is one
// line per file of the import set: path relative to the install, '/' between components, a tab,
// the size in bytes, a tab, 64 hex digits of SHA-256. A row whose size and hash are both empty is
// unchecked: the file must be there, but its content is not known, so only the rules that need no
// reference apply (it is not empty; a BMP is as long as its own header says). Blank lines and
// lines starting with '#' are skipped. It holds no byte of any file.
//
// This is the headless core: the command line (`--import DIR`, ImportCommand.h) and the launcher
// both call importGame and print or draw its progress and report. verifyImportedGame checks an
// imported copy again, on demand, without changing it (`--verify-import`, the launcher's verify).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "app/ImportFiles.h"

namespace sfcop::app {

// The manifest's name; it sits beside the hash list (Bundle.h), wherever that is.
inline constexpr const char kInstallManifestFile[] = "install_manifest.tsv";

// The size of the 2.5.0.0 executable, which the disc installs before the official patch.
inline constexpr std::uint64_t kExeSize2500 = 8122368;

// How many folder levels below the chosen folder the search for the executable goes: enough for
// a Steam compatdata prefix (compatdata/<id>/pfx/drive_c/Program Files (x86)/<company>/<game>).
inline constexpr int kSearchDepth = 8;

// The record an import leaves in the game folder (one "key<TAB>value" per line).
inline constexpr const char kImportRecordFile[] = ".sfcop-import.txt";

struct ManifestEntry {
    std::string path;
    std::uint64_t size = 0;
    // 64 lowercase hex digits; empty for an unchecked row (size 0 then).
    std::string sha256;

    bool checked() const { return !sha256.empty(); }
};

struct InstallManifest {
    std::vector<ManifestEntry> entries;
    // Empty when the manifest is good; otherwise "line N: what is wrong".
    std::string error;

    bool ok() const { return error.empty(); }
    // The entry of the game executable at the root; null when the manifest has none.
    const ManifestEntry *exe() const;
};

// Refuses a malformed line, a path that leaves the root or is listed twice (ignoring case, as the
// game's own file system does), and a manifest without the game executable at its root, checked.
InstallManifest parseInstallManifest(const std::string &text);

// The manifest beside the hash list `hashListPath`.
std::string installManifestBeside(const std::string &hashListPath);

enum class ImportPhase {
    kSearching,     // looking for the executable in and below the chosen folder
    kCheckingGame,  // hashing the executable, then sizing the import set
    kCopying,       // copying and hashing; bytesDone/bytesTotal count the import set
    kFinishing,     // writing the record and moving the copy into place
    kVerifying,     // verifyImportedGame hashing the copy; bytesDone/bytesTotal count those bytes
};

struct ImportProgress {
    ImportPhase phase = ImportPhase::kSearching;
    std::size_t fileIndex = 0; // 0-based, during kCopying and kVerifying
    std::size_t fileCount = 0;
    // The file being copied or hashed, relative to the install.
    std::string path;
    std::uint64_t bytesDone = 0;
    std::uint64_t bytesTotal = 0;
};

struct ImportCallbacks {
    // Called at each phase and after every piece copied (at most 1 MiB).
    std::function<void(const ImportProgress &)> progress;
    // Polled between pieces; true stops the import, which then removes what it wrote.
    std::function<bool()> cancelled;
};

struct ImportRequest {
    // The folder the player chose.
    std::string source;
    // Where the imported game goes (DataDir.h, gameDirFor); it may exist from an earlier import.
    std::string gameDir;
    InstallManifest manifest;
};

enum class ImportStatus {
    kImported,
    kNotFound,          // no executable in or below the folder
    kDiscOrInstaller,   // the disc, an image of it, an installer or an archive
    kVersion2500,       // the executable is 2.5.0.0
    kExeModified,       // the executable has 2.5.5.2's size and another hash
    kNotTheGame,        // an executable of another size
    kDamaged,           // files missing, empty or shorter than in 2.5.5.2
    kNoSpace,
    kCancelled,
    kWriteFailed,       // the copy could not be written or moved into place
    kBadManifest,
    kOwnCopy,           // import only: the folder, or a link in it, is or holds the client's copy
    kTooManyFolders,    // files not found, and the install too big or deep to look at all
    kVerified,          // verifyImportedGame: every file is there and whole (changed ones listed)
};

struct DamagedFile {
    std::string path;
    // "missing", "empty" or "truncated"; verifyImportedGame also "unreadable"
    std::string problem;
    std::uint64_t size = 0;
    std::uint64_t expectedSize = 0;
};

struct ImportReport {
    ImportStatus status = ImportStatus::kImported;
    // The folder the player chose, and the install found in or below it.
    std::string source;
    std::string installDir;
    std::string gameDir;
    // kExeModified, kNotTheGame, kVersion2500: the executable; kWriteFailed: what failed.
    std::string path;
    std::string manifestError;
    std::vector<DamagedFile> damaged;
    // Imported as they are: files whose hash differs from 2.5.5.2's.
    std::vector<std::string> changed;
    // Imported: mission scripts (.scr under Assets/Scripts) the manifest does not list.
    std::vector<std::string> modScripts;
    std::uint64_t bytesNeeded = 0;
    std::uint64_t bytesFree = 0;
    std::size_t filesCopied = 0;
    std::uint64_t bytesCopied = 0;
    // How many files were cloned rather than copied.
    std::size_t filesCloned = 0;
    // When the import failed with the earlier import set aside and could not put it back: where
    // it is (`<game>.old`); the next import puts it back first.
    std::string earlierAt;
    // When the install found holds a link into the client's own copy (kOwnCopy): the link, as a
    // path in the install.
    std::string ownCopyLink;
    // verifyImportedGame: true, and how many files it hashed (the executable, then each checked
    // row of the manifest).
    bool verify = false;
    std::size_t filesHashed = 0;
    std::uint64_t bytesHashed = 0;

    bool imported() const { return status == ImportStatus::kImported; }
    // 0 imported or verified; 3 the folder is refused (not found, disc, wrong or damaged game); 1
    // the import or the check could not finish (space, a write, a cancel, the manifest).
    int exitCode() const;
    // For the player: what happened, every file at fault by name, and what to do.
    std::string message() const;
};

ImportReport importGame(const ImportRequest &request, ImportFiles &files,
                        const ImportCallbacks &callbacks = {});

// Checks the imported copy in `gameDir` again and changes nothing: its executable as the import
// checks one, then every file of the manifest there (matched ignoring case), not empty, not
// shorter than in 2.5.5.2, a BMP of an unchecked row as long as its header says, and each checked
// row hashed (kVerifying progress; cancel polled between pieces). A file the survey refuses ends
// the check before any hashing; mod scripts and unchecked rows are sized, not read. The copy's
// own links are followed; the import's own-copy rules do not apply. The report has the import's
// shape: kVerified with `changed` (a hash other than the manifest's, whether imported so or
// changed since) and `modScripts`; kNotFound when there is no copy; the executable's statuses;
// kDamaged with every file at fault ("unreadable" for one that cannot be read through);
// kTooManyFolders; kCancelled; kBadManifest. For the launcher's verify action and `--verify-import`.
ImportReport verifyImportedGame(const std::string &gameDir, const InstallManifest &manifest,
                                ImportFiles &files, const ImportCallbacks &callbacks = {});

// What the record of an import says was accepted though it may differ from 2.5.5.2: the files
// imported with a hash other than the manifest's ("changed", warned about at the import) and the
// files of unchecked manifest rows ("unchecked", not compared). Paths as the manifest names them.
// A run that starts from the imported copy accepts these files as the import did (Startup.h).
struct ImportRecord {
    std::vector<std::string> changed;
    std::vector<std::string> unchecked;
    // The install the copy was made from, and how many files and bytes were copied: what the
    // launcher shows beside the game (empty and 0 when the record does not say).
    std::string source;
    std::size_t files = 0;
    std::uint64_t bytes = 0;
};

ImportRecord parseImportRecord(const std::string &text);

} // namespace sfcop::app
