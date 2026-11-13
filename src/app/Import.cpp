#include "app/Import.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <set>

#include "app/ExitCodes.h"
#include "app/Install.h"
#include "app/Sha256.h"

namespace sfcop::app {

namespace {

constexpr std::size_t kHexDigits = 64;
constexpr std::size_t kPiece = 1 << 20;
// Folders a search or a listing of the install looks at, at most: a player who chooses their
// home folder gets an answer in seconds rather than a walk of the whole disk.
constexpr std::size_t kMaxFolders = 20000;
// Installs found below the chosen folder that are looked at, at most.
constexpr std::size_t kMaxCandidates = 8;
// How deep the index of the install found goes, every folder counted, links followed.
constexpr int kMaxInstallDepth = 16;
// The mission scripts' folder, in lower case; a .scr in it that the manifest does not list is a
// mod script.
constexpr const char kScriptsFolder[] = "assets/scripts/";
constexpr const char kScriptsFolderCanonical[] = "Assets/Scripts/";
constexpr const char kPartialSuffix[] = ".partial";
constexpr const char kOldSuffix[] = ".old";

// What a player may choose by mistake: the disc's image, an installer, the patch, an archive.
constexpr const char *kNotInstalledExtensions[] = {".iso", ".img", ".bin", ".cue", ".mdf",
                                                   ".nrg", ".dmg", ".exe", ".msi", ".rtp",
                                                   ".zip", ".7z",  ".rar"};

std::string lower(std::string text) {
    for (char &c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

bool endsWith(const std::string &text, const std::string &tail) {
    return text.size() >= tail.size() &&
           text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

bool isHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Whether `path` is `folder` or inside it; both resolved (ImportFiles::resolved). Case is
// ignored on every platform, so that a differently spelt path is no way round on the
// case-insensitive volumes of Windows and macOS. On a case-sensitive volume (Linux) this
// over-refuses: a folder that differs from the client's copy only in case is refused too.
bool isWithin(const std::string &path, const std::string &folder) {
    const std::string p = lower(path);
    const std::string f = lower(folder);
    return p == f || (p.size() > f.size() && p.compare(0, f.size(), f) == 0 &&
                      (p[f.size()] == '/' || (!f.empty() && f.back() == '/')));
}

// A path that stays inside the root: relative, '/' separated, no empty, "." or ".." component.
bool isInsideRoot(const std::string &path) {
    if (path.empty() || path[0] == '/' || path.find('\\') != std::string::npos ||
        path.find(':') != std::string::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find('/', start);
        if (end == std::string::npos) {
            end = path.size();
        }
        const std::string part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        start = end + 1;
    }
    return true;
}

std::string lineError(int line, const std::string &what) {
    return "line " + std::to_string(line) + ": " + what;
}

std::string parentOf(const std::string &path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string megabytes(std::uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f MB", static_cast<double>(bytes) / 1e6);
    return text;
}

class Importer {
public:
    Importer(const ImportRequest &request, ImportFiles &files, const ImportCallbacks &callbacks)
        : request_(request), files_(files), callbacks_(callbacks) {
        report_.source = request.source;
        report_.gameDir = request.gameDir;
    }

    ImportReport run() {
        if (!request_.manifest.ok() || request_.manifest.exe() == nullptr) {
            report_.manifestError = request_.manifest.ok()
                                        ? std::string("it names no ") + kGameExe
                                        : request_.manifest.error;
            return finish(ImportStatus::kBadManifest);
        }
        announce(ImportPhase::kSearching);
        if (!locate()) {
            return report_;
        }
        announce(ImportPhase::kCheckingGame);
        if (!checkExe() || !survey()) {
            return report_;
        }
        if (!checkSpace()) {
            return report_;
        }
        if (!copyAll()) {
            files_.removeAll(partial());
            return report_;
        }
        announce(ImportPhase::kFinishing);
        if (!writeRecord() || !moveIntoPlace()) {
            files_.removeAll(partial());
            return report_;
        }
        return finish(ImportStatus::kImported);
    }

    // The copy in request_.gameDir checked in place: nothing is written.
    ImportReport verify() {
        report_.verify = true;
        report_.installDir = request_.gameDir;
        if (!request_.manifest.ok() || request_.manifest.exe() == nullptr) {
            report_.manifestError = request_.manifest.ok()
                                        ? std::string("it names no ") + kGameExe
                                        : request_.manifest.error;
            return finish(ImportStatus::kBadManifest);
        }
        announce(ImportPhase::kCheckingGame);
        if (files_.kindFollowing(request_.gameDir) != EntryKind::kFolder ||
            !exePath(request_.gameDir)) {
            return finish(ImportStatus::kNotFound);
        }
        if (!checkExe() || !survey()) {
            return report_;
        }
        // The executable was hashed by checkExe; a mod script and an unchecked row have no hash.
        // The progress totals cover the files hashed here; the report's counts include the
        // executable too, so they are its size and one file more.
        std::vector<const Item *> hashed;
        progress_.bytesTotal = 0;
        for (const Item &item : items_) {
            if (item.entry != nullptr && item.entry->checked() &&
                item.entry != request_.manifest.exe()) {
                hashed.push_back(&item);
                progress_.bytesTotal += item.size;
            }
        }
        progress_.fileCount = hashed.size();
        report_.filesHashed = 1;
        report_.bytesHashed = request_.manifest.exe()->size;
        announce(ImportPhase::kVerifying);
        std::vector<unsigned char> piece(kPiece);
        for (std::size_t i = 0; i < hashed.size(); ++i) {
            const Item &item = *hashed[i];
            progress_.fileIndex = i;
            progress_.path = item.to;
            const std::unique_ptr<FileReader> reader = files_.openRead(item.from);
            std::uint64_t size = 0;
            std::string hash;
            const CopyOutcome outcome =
                reader ? pump(*reader, nullptr, piece, size, hash) : CopyOutcome::kFailed;
            if (outcome == CopyOutcome::kCancelled) {
                return finish(ImportStatus::kCancelled);
            }
            ++report_.filesHashed;
            report_.bytesHashed += size;
            if (outcome == CopyOutcome::kFailed) {
                report_.damaged.push_back({item.to, "unreadable", size, item.entry->size});
            } else if (size < item.entry->size) {
                // Shortened between the survey and the read: damage, as in the survey.
                report_.damaged.push_back({item.to, "truncated", size, item.entry->size});
            } else if (hash != item.entry->sha256) {
                report_.changed.push_back(item.to);
            }
        }
        return finish(report_.damaged.empty() ? ImportStatus::kVerified : ImportStatus::kDamaged);
    }

private:
    struct Item {
        // In the install, and in the game folder.
        std::string from;
        std::string to;
        // Null for a mod script.
        const ManifestEntry *entry = nullptr;
        std::uint64_t size = 0;
    };

    ImportReport &finish(ImportStatus status, std::string path = {}) {
        report_.status = status;
        if (!path.empty()) {
            report_.path = std::move(path);
        }
        return report_;
    }

    bool cancelled() const { return callbacks_.cancelled && callbacks_.cancelled(); }

    void announce(ImportPhase phase) {
        progress_.phase = phase;
        if (callbacks_.progress) {
            callbacks_.progress(progress_);
        }
    }

    std::string partial() const { return request_.gameDir + kPartialSuffix; }
    std::string old() const { return request_.gameDir + kOldSuffix; }

    // The file named `name` in `dir`, ignoring case: the first in byte order when several match.
    std::optional<std::string> findIgnoringCase(const std::string &dir, const std::string &name,
                                                EntryKind kind) const {
        const std::string wanted = lower(name);
        for (const FolderEntry &entry : files_.list(dir)) {
            if (lower(entry.name) == wanted && kindOf(dir, entry) == kind) {
                return entry.name;
            }
        }
        return std::nullopt;
    }

    // The entry's kind, a symbolic link taken as what it points at. The search below the chosen
    // folder never follows a link to a folder (a Wine prefix links the whole disk); a link to a
    // file, and every link inside the install found, is followed.
    std::optional<EntryKind> kindOf(const std::string &dir, const FolderEntry &entry) const {
        if (entry.kind != EntryKind::kOther) {
            return entry.kind;
        }
        return files_.kindFollowing(joinPath(dir, entry.name));
    }

    bool isNotInstalledFile(const std::string &path) const {
        const std::string name = lower(path);
        for (const char *extension : kNotInstalledExtensions) {
            if (endsWith(name, extension)) {
                return true;
            }
        }
        return false;
    }

    // An installer's folder (the disc, or a copy of it): it holds InstallShield cabinets.
    bool looksLikeInstaller(const std::string &dir) const {
        for (const FolderEntry &entry : files_.list(dir)) {
            if (entry.kind == EntryKind::kFile && endsWith(lower(entry.name), ".cab")) {
                return true;
            }
        }
        return false;
    }

    // Whether `dir`, links resolved, is the game folder, the unfinished or the set-aside copy, or
    // inside one: the import removes the last two before it copies, so one of them as the source
    // would be deleted under it.
    bool isOwnCopy(const std::string &dir) const {
        const std::string where = files_.resolved(dir);
        for (const std::string &own : ownCopies()) {
            if (isWithin(where, own)) {
                return true;
            }
        }
        return false;
    }

    // Whether `target`, resolved, is in the client's own copy or holds it.
    bool touchesOwnCopy(const std::string &target) const {
        for (const std::string &where : ownCopies()) {
            if (isWithin(target, where) || isWithin(where, target)) {
                return true;
            }
        }
        return false;
    }

    // The client's own copy, its unfinished and its set-aside one, resolved once.
    const std::vector<std::string> &ownCopies() const {
        if (ownCopies_.empty()) {
            for (const std::string &own : {request_.gameDir, partial(), old()}) {
                ownCopies_.push_back(files_.resolved(own));
            }
        }
        return ownCopies_;
    }

    // Finds the install: the chosen folder, or the first folder below it, breadth first and in
    // name order, whose executable is 2.5.5.2; failing that the first that holds one at all.
    bool locate() {
        if (isOwnCopy(request_.source)) {
            finish(ImportStatus::kOwnCopy);
            return false;
        }
        // The chosen folder may itself be a link.
        const std::optional<EntryKind> kind = files_.kindFollowing(request_.source);
        if (!kind) {
            finish(ImportStatus::kNotFound);
            return false;
        }
        if (*kind != EntryKind::kFolder) {
            finish(isNotInstalledFile(request_.source) ? ImportStatus::kDiscOrInstaller
                                                       : ImportStatus::kNotFound);
            return false;
        }
        std::vector<std::string> candidates;
        bool installer = false;
        std::deque<std::pair<std::string, int>> queue = {{request_.source, 0}};
        std::size_t visited = 0;
        while (!queue.empty() && visited < kMaxFolders && candidates.size() < kMaxCandidates) {
            if (cancelled()) {
                finish(ImportStatus::kCancelled);
                return false;
            }
            const auto [dir, depth] = queue.front();
            queue.pop_front();
            ++visited;
            const std::vector<FolderEntry> entries = files_.list(dir);
            bool holdsExe = false;
            for (const FolderEntry &entry : entries) {
                holdsExe = holdsExe || (lower(entry.name) == lower(kGameExe) &&
                                        kindOf(dir, entry) == EntryKind::kFile);
            }
            if (holdsExe) {
                // The client's own copy below the chosen folder (the data folder chosen) is
                // passed over, as if it were not there.
                if (!isOwnCopy(dir)) {
                    candidates.push_back(dir);
                }
                continue;
            }
            installer = installer || (depth == 0 && looksLikeInstaller(dir));
            if (depth < kSearchDepth) {
                for (const FolderEntry &entry : entries) {
                    const std::string sub = joinPath(dir, entry.name);
                    // An unfinished or set-aside import of our own is never a source.
                    if (entry.kind == EntryKind::kFolder && sub != partial() && sub != old()) {
                        queue.emplace_back(sub, depth + 1);
                    }
                }
            }
        }
        if (candidates.empty()) {
            finish(installer ? ImportStatus::kDiscOrInstaller : ImportStatus::kNotFound);
            return false;
        }
        report_.installDir = candidates.front();
        for (const std::string &dir : candidates) {
            const std::optional<std::string> exe = exePath(dir);
            if (exe && hashOf(*exe) == request_.manifest.exe()->sha256) {
                report_.installDir = dir;
                break;
            }
        }
        return true;
    }

    std::optional<std::string> exePath(const std::string &dir) const {
        const std::optional<std::string> name = findIgnoringCase(dir, kGameExe, EntryKind::kFile);
        if (!name) {
            return std::nullopt;
        }
        return joinPath(dir, *name);
    }

    // The file's SHA-256 in hex; empty when it cannot be read or the run is cancelled.
    std::string hashOf(const std::string &path) const {
        const std::unique_ptr<FileReader> reader = files_.openRead(path);
        if (!reader) {
            return std::string();
        }
        std::vector<unsigned char> piece(kPiece);
        Sha256 hash;
        while (const std::size_t got = reader->read(piece.data(), piece.size())) {
            if (cancelled()) {
                return std::string();
            }
            hash.update(piece.data(), got);
        }
        return reader->failed() ? std::string() : Sha256::toHex(hash.finish());
    }

    bool checkExe() {
        if (cancelled()) {
            finish(ImportStatus::kCancelled);
            return false;
        }
        const ManifestEntry &expected = *request_.manifest.exe();
        const std::optional<std::string> exe = exePath(report_.installDir);
        const std::uint64_t size = exe ? files_.fileSize(*exe).value_or(0) : 0;
        const std::string path = exe ? *exe : joinPath(report_.installDir, kGameExe);
        if (size == kExeSize2500) {
            finish(ImportStatus::kVersion2500, path);
            return false;
        }
        if (size != expected.size) {
            finish(ImportStatus::kNotTheGame, path);
            return false;
        }
        const std::string hash = hashOf(path);
        if (cancelled()) {
            finish(ImportStatus::kCancelled);
            return false;
        }
        if (hash != expected.sha256) {
            finish(ImportStatus::kExeModified, path);
            return false;
        }
        return true;
    }

    // Every file of the install, keyed by its path in lower case; the first in byte order wins
    // when two differ only in case.
    // Links are followed, to a bounded depth. A link back to a folder above is passed over:
    // what it leads to is listed already. For an import, a folder that is the client's own copy
    // (the data folder inside the install) is passed over too, and a link into the copy or to a
    // folder holding it refuses the install; a check lists the copy itself, so neither applies.
    void index(const std::string &dir, const std::string &relative, std::size_t &folders,
               int depth = 0) {
        if (++folders > kMaxFolders || depth > kMaxInstallDepth) {
            indexCut_ = true;
            return;
        }
        for (const FolderEntry &entry : files_.list(dir)) {
            const std::string path = relative.empty() ? entry.name : relative + "/" + entry.name;
            const std::optional<EntryKind> kind = kindOf(dir, entry);
            if (kind == EntryKind::kFolder && entry.kind == EntryKind::kFolder &&
                !report_.verify && isOwnCopy(joinPath(dir, entry.name))) {
                continue; // removed before the copy, and never part of the game
            }
            if (entry.kind == EntryKind::kOther && kind) {
                const std::string target = files_.resolved(joinPath(dir, entry.name));
                // A link into the client's own copy, or to a folder holding it, would be read
                // after the import removed it.
                if (!report_.verify && touchesOwnCopy(target)) {
                    if (report_.ownCopyLink.empty()) {
                        report_.ownCopyLink = path;
                    }
                    continue;
                }
                if (kind == EntryKind::kFolder && isWithin(files_.resolved(dir), target)) {
                    continue;
                }
            }
            if (kind == EntryKind::kFile) {
                files_by_lower_.emplace(lower(path), path);
            } else if (kind == EntryKind::kFolder) {
                index(joinPath(dir, entry.name), path, folders, depth + 1);
            }
        }
    }

    // Sizes the import set and refuses missing, empty and short files before anything is
    // written.
    bool survey() {
        std::size_t folders = 0;
        index(report_.installDir, std::string(), folders);
        if (!report_.ownCopyLink.empty()) {
            finish(ImportStatus::kOwnCopy);
            return false;
        }
        std::set<std::string> listed;
        for (const ManifestEntry &entry : request_.manifest.entries) {
            listed.insert(lower(entry.path));
            const auto found = files_by_lower_.find(lower(entry.path));
            if (found == files_by_lower_.end()) {
                report_.damaged.push_back({entry.path, "missing", 0, entry.size});
                continue;
            }
            Item item;
            item.from = joinPath(report_.installDir, found->second);
            item.to = entry.path;
            item.entry = &entry;
            item.size = files_.fileSize(item.from).value_or(0);
            if (!damagedBySize(entry.path, item.from, item.size, &entry)) {
                items_.push_back(std::move(item));
            }
        }
        for (const auto &[key, path] : files_by_lower_) {
            if (key.rfind(kScriptsFolder, 0) != 0 || !endsWith(key, ".scr") || listed.count(key)) {
                continue;
            }
            Item item;
            item.from = joinPath(report_.installDir, path);
            item.to = kScriptsFolderCanonical + path.substr(sizeof kScriptsFolder - 1);
            item.size = files_.fileSize(item.from).value_or(0);
            if (!damagedBySize(item.to, item.from, item.size, nullptr)) {
                report_.modScripts.push_back(item.to);
                items_.push_back(std::move(item));
            }
        }
        // A listing cut short at a limit (a link that loops is cut, harmlessly, when every file
        // was found above it) makes the files past it read as missing: say what happened.
        if (indexCut_ && std::any_of(report_.damaged.begin(), report_.damaged.end(),
                                     [](const DamagedFile &f) { return f.problem == "missing"; })) {
            finish(ImportStatus::kTooManyFolders);
            return false;
        }
        if (!report_.damaged.empty()) {
            finish(ImportStatus::kDamaged);
            return false;
        }
        for (const Item &item : items_) {
            progress_.bytesTotal += item.size;
        }
        progress_.fileCount = items_.size();
        return true;
    }

    // An empty file, or one shorter than 2.5.5.2's, is damaged (an install copied off a failing
    // disc or drive loses the ends of files). `entry` is null for a mod script, which is damaged
    // only when empty; an unchecked entry is damaged when empty, or, for a BMP, when shorter than
    // its own header says (read from `readFrom`).
    bool damagedBySize(const std::string &path, const std::string &readFrom, std::uint64_t size,
                       const ManifestEntry *entry) {
        const bool checked = entry != nullptr && entry->checked();
        const std::uint64_t expected = checked ? entry->size : 0;
        if (size == 0 && (!checked || expected > 0)) {
            report_.damaged.push_back({path, "empty", 0, expected});
            return true;
        }
        if (size < expected) {
            report_.damaged.push_back({path, "truncated", size, expected});
            return true;
        }
        if (entry != nullptr && !checked && endsWith(lower(path), ".bmp")) {
            const std::uint64_t declared = bmpDeclaredSize(readFrom);
            if (size < declared) {
                report_.damaged.push_back({path, "truncated", size, declared});
                return true;
            }
        }
        return false;
    }

    // The file size a BMP's header gives (bytes 2 to 5, little-endian, after "BM"); 0 when the
    // file does not start like a BMP or cannot be read.
    std::uint64_t bmpDeclaredSize(const std::string &path) const {
        std::unique_ptr<FileReader> reader = files_.openRead(path);
        unsigned char head[6] = {};
        std::size_t got = 0;
        while (reader && got < sizeof head) {
            const std::size_t n = reader->read(head + got, sizeof head - got);
            if (n == 0) {
                break;
            }
            got += n;
        }
        if (got < sizeof head || head[0] != 'B' || head[1] != 'M') {
            return 0;
        }
        return std::uint64_t(head[2]) | std::uint64_t(head[3]) << 8 |
               std::uint64_t(head[4]) << 16 | std::uint64_t(head[5]) << 24;
    }

    bool checkSpace() {
        report_.bytesNeeded = progress_.bytesTotal + progress_.bytesTotal / 10;
        const std::optional<std::uint64_t> free = files_.freeBytes(request_.gameDir);
        report_.bytesFree = free.value_or(0);
        if (free && *free < report_.bytesNeeded) {
            finish(ImportStatus::kNoSpace, parentOf(request_.gameDir));
            return false;
        }
        return true;
    }

    bool copyAll() {
        // A run that stopped between moving the earlier import aside and moving the new one in
        // left it only as the old folder: put it back rather than remove it.
        if (!files_.kindOf(request_.gameDir) && files_.kindOf(old()) &&
            !files_.rename(old(), request_.gameDir)) {
            // The earlier import is still only there: leave it for the next attempt.
            report_.earlierAt = old();
            finish(ImportStatus::kWriteFailed, request_.gameDir);
            return false;
        }
        if (!files_.removeAll(partial()) || !files_.removeAll(old()) ||
            !files_.createDirectories(partial())) {
            finish(ImportStatus::kWriteFailed, partial());
            return false;
        }
        announce(ImportPhase::kCopying);
        std::vector<unsigned char> piece(kPiece);
        for (std::size_t i = 0; i < items_.size(); ++i) {
            const Item &item = items_[i];
            progress_.fileIndex = i;
            progress_.path = item.to;
            const std::string to = joinPath(partial(), item.to);
            if (!files_.createDirectories(parentOf(to))) {
                finish(ImportStatus::kWriteFailed, parentOf(to));
                return false;
            }
            std::uint64_t size = 0;
            std::string hash;
            const CopyOutcome outcome = copyOne(item.from, to, piece, size, hash);
            if (outcome == CopyOutcome::kCancelled) {
                finish(ImportStatus::kCancelled);
                return false;
            }
            if (outcome == CopyOutcome::kFailed) {
                finish(ImportStatus::kWriteFailed, to);
                return false;
            }
            ++report_.filesCopied;
            report_.bytesCopied += size;
            if (damagedBySize(item.to, to, size, item.entry)) {
                continue;
            }
            if (item.entry != nullptr && item.entry->checked() && hash != item.entry->sha256) {
                report_.changed.push_back(item.to);
            }
        }
        if (!report_.damaged.empty()) {
            finish(ImportStatus::kDamaged);
            return false;
        }
        return true;
    }

    enum class CopyOutcome { kCopied, kCancelled, kFailed };

    // Clones `from` to `to` and hashes the clone as read back, or copies it piece by piece,
    // hashing each piece as it is handed to the writer (the bytes read from the install; a write
    // the system reports as failed stops the import, but the copy is not read back).
    CopyOutcome copyOne(const std::string &from, const std::string &to,
                        std::vector<unsigned char> &piece, std::uint64_t &size, std::string &hex) {
        const bool cloned = files_.clone(from, to);
        report_.filesCloned += cloned ? 1 : 0;
        const std::unique_ptr<FileReader> reader = files_.openRead(cloned ? to : from);
        const std::unique_ptr<FileWriter> writer = cloned ? nullptr : files_.openWrite(to);
        if (!reader || (!cloned && !writer)) {
            return CopyOutcome::kFailed;
        }
        return pump(*reader, writer.get(), piece, size, hex);
    }

    // Reads `reader` to its end, handing each piece to `writer` when there is one, hashing it and
    // reporting progress; cancel is polled between pieces.
    CopyOutcome pump(FileReader &reader, FileWriter *writer, std::vector<unsigned char> &piece,
                     std::uint64_t &size, std::string &hex) {
        Sha256 hash;
        while (const std::size_t got = reader.read(piece.data(), piece.size())) {
            if (cancelled()) {
                return CopyOutcome::kCancelled;
            }
            if (writer && !writer->write(piece.data(), got)) {
                return CopyOutcome::kFailed;
            }
            hash.update(piece.data(), got);
            size += got;
            progress_.bytesDone += got;
            if (callbacks_.progress) {
                callbacks_.progress(progress_);
            }
        }
        if (reader.failed() || (writer && !writer->close())) {
            return CopyOutcome::kFailed;
        }
        if (cancelled()) {
            return CopyOutcome::kCancelled;
        }
        hex = Sha256::toHex(hash.finish());
        return CopyOutcome::kCopied;
    }

    bool writeRecord() {
        std::string text = "# What sfcop --import copied here. Not read by the game.\n";
        text += "manifest-files\t" + std::to_string(request_.manifest.entries.size()) + "\n";
        text += "source\t" + report_.installDir + "\n";
        text += "files\t" + std::to_string(report_.filesCopied) + "\n";
        text += "bytes\t" + std::to_string(report_.bytesCopied) + "\n";
        for (const std::string &path : report_.changed) {
            text += "changed\t" + path + "\n";
        }
        for (const std::string &path : report_.modScripts) {
            text += "mod-script\t" + path + "\n";
        }
        for (const Item &item : items_) {
            if (item.entry != nullptr && !item.entry->checked()) {
                text += "unchecked\t" + item.to + "\n";
            }
        }
        const std::string path = joinPath(partial(), kImportRecordFile);
        const std::unique_ptr<FileWriter> writer = files_.openWrite(path);
        if (!writer ||
            !writer->write(reinterpret_cast<const unsigned char *>(text.data()), text.size()) ||
            !writer->close()) {
            finish(ImportStatus::kWriteFailed, path);
            return false;
        }
        return true;
    }

    // The verified copy replaces the game folder; an earlier import is kept until the new one is
    // in place, and restored when the move fails.
    bool moveIntoPlace() {
        const bool earlier = files_.kindOf(request_.gameDir).has_value();
        if (earlier && !files_.rename(request_.gameDir, old())) {
            finish(ImportStatus::kWriteFailed, request_.gameDir);
            return false;
        }
        if (!files_.rename(partial(), request_.gameDir)) {
            if (earlier && !files_.rename(old(), request_.gameDir)) {
                report_.earlierAt = old();
            }
            finish(ImportStatus::kWriteFailed, request_.gameDir);
            return false;
        }
        if (earlier) {
            files_.removeAll(old());
        }
        return true;
    }

    const ImportRequest &request_;
    ImportFiles &files_;
    const ImportCallbacks &callbacks_;
    ImportReport report_;
    ImportProgress progress_;
    std::map<std::string, std::string> files_by_lower_;
    std::vector<Item> items_;
    // Whether index() stopped at a limit.
    bool indexCut_ = false;
    mutable std::vector<std::string> ownCopies_;
};

} // namespace

const ManifestEntry *InstallManifest::exe() const {
    for (const ManifestEntry &entry : entries) {
        if (entry.path == kGameExe) {
            return &entry;
        }
    }
    return nullptr;
}

ImportRecord parseImportRecord(const std::string &text) {
    ImportRecord record;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos || tab + 1 == line.size()) {
            continue;
        }
        const std::string key = line.substr(0, tab);
        if (key == "changed") {
            record.changed.push_back(line.substr(tab + 1));
        } else if (key == "unchecked") {
            record.unchecked.push_back(line.substr(tab + 1));
        } else if (key == "source") {
            record.source = line.substr(tab + 1);
        } else if (key == "files" || key == "bytes") {
            const std::string value = line.substr(tab + 1);
            if (value.find_first_not_of("0123456789") == std::string::npos && value.size() <= 19) {
                const std::uint64_t number = std::stoull(value);
                if (key == "files") {
                    record.files = static_cast<std::size_t>(number);
                } else {
                    record.bytes = number;
                }
            }
        }
    }
    return record;
}

InstallManifest parseInstallManifest(const std::string &text) {
    InstallManifest manifest;
    std::map<std::string, int> firstLine;
    std::size_t start = 0;
    int line = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string row = text.substr(start, end - start);
        start = end + 1;
        ++line;
        if (!row.empty() && row.back() == '\r') {
            row.pop_back();
        }
        if (row.empty() || row[0] == '#') {
            continue;
        }
        const std::size_t tab1 = row.find('\t');
        const std::size_t tab2 = tab1 == std::string::npos ? tab1 : row.find('\t', tab1 + 1);
        if (tab2 == std::string::npos || row.find('\t', tab2 + 1) != std::string::npos) {
            manifest.error = lineError(line, "expected a path, a size and a SHA-256, separated "
                                             "by tabs");
            return manifest;
        }
        ManifestEntry entry;
        entry.path = row.substr(0, tab1);
        const std::string size = row.substr(tab1 + 1, tab2 - tab1 - 1);
        const std::string hash = row.substr(tab2 + 1);
        if (!isInsideRoot(entry.path)) {
            manifest.error = lineError(line, "the path '" + entry.path +
                                                 "' must be relative, with '/' between parts, "
                                                 "and stay inside the root");
            return manifest;
        }
        const bool unchecked = size.empty() && hash.empty();
        if (!unchecked && (size.empty() || size.size() > 12 ||
                           size.find_first_not_of("0123456789") != std::string::npos)) {
            manifest.error = lineError(line, "the size '" + size + "' is not a whole number");
            return manifest;
        }
        if (!unchecked && hash.size() != kHexDigits) {
            manifest.error = lineError(line, "expected 64 hex digits of SHA-256");
            return manifest;
        }
        for (char c : hash) {
            if (!isHex(c)) {
                manifest.error = lineError(line, "expected 64 hex digits of SHA-256");
                return manifest;
            }
        }
        if (unchecked && entry.path == kGameExe) {
            manifest.error = lineError(line, std::string("the row of ") + kGameExe +
                                                 " must give its size and SHA-256");
            return manifest;
        }
        const auto [previous, isNew] = firstLine.emplace(lower(entry.path), line);
        if (!isNew) {
            manifest.error = lineError(line, "'" + entry.path + "' is listed twice (first on line " +
                                                 std::to_string(previous->second) + ")");
            return manifest;
        }
        entry.size = unchecked ? 0 : std::stoull(size);
        entry.sha256 = lower(hash);
        manifest.entries.push_back(std::move(entry));
    }
    if (manifest.exe() == nullptr) {
        manifest.error = std::string("the manifest names no ") + kGameExe + " at its root";
    }
    return manifest;
}

std::string installManifestBeside(const std::string &hashListPath) {
    const std::string dir = parentOf(hashListPath);
    return dir.empty() ? std::string(kInstallManifestFile) : joinPath(dir, kInstallManifestFile);
}

int ImportReport::exitCode() const {
    switch (status) {
    case ImportStatus::kImported:
    case ImportStatus::kVerified:
        return kExitSuccess;
    case ImportStatus::kNotFound:
    case ImportStatus::kDiscOrInstaller:
    case ImportStatus::kVersion2500:
    case ImportStatus::kExeModified:
    case ImportStatus::kNotTheGame:
    case ImportStatus::kDamaged:
    case ImportStatus::kOwnCopy:
    case ImportStatus::kTooManyFolders:
        return kExitInstallRefused;
    case ImportStatus::kNoSpace:
    case ImportStatus::kCancelled:
    case ImportStatus::kWriteFailed:
    case ImportStatus::kBadManifest:
        break;
    }
    return kExitFailure;
}

std::string ImportReport::message() const {
    const std::string reinstall = "Install Starfleet Command: Orion Pirates and the official "
                                  "2.5.5.2 patch on Windows, or under Wine or CrossOver, then "
                                  "import the folder it was installed in.";
    std::string text;
    if (verify) {
        switch (status) {
        case ImportStatus::kVerified:
            text = "The imported game in " + gameDir + " is complete: " +
                   std::to_string(filesHashed) + " files (" + megabytes(bytesHashed) +
                   ") checked against the 2.5.5.2 release.";
            if (!changed.empty()) {
                text += "\nThese files differ from the 2.5.5.2 release (imported so, or changed "
                        "since):";
                for (const std::string &file : changed) {
                    text += "\n  " + file;
                }
            }
            for (const std::string &file : modScripts) {
                text += "\n  mod script: " + file;
            }
            return text;
        case ImportStatus::kNotFound:
            return "There is no imported game in " + gameDir + ". Import the game first.";
        case ImportStatus::kDamaged:
            text = "The imported game in " + gameDir +
                   " is damaged: these files are missing, empty, shorter than in the 2.5.5.2 "
                   "release or unreadable:";
            for (const DamagedFile &file : damaged) {
                text += "\n  " + file.problem + ": " + file.path;
                if (file.problem == "truncated") {
                    text += " (" + std::to_string(file.size) + " of " +
                            std::to_string(file.expectedSize) + " bytes)";
                }
            }
            return text + "\nImport the game again from the installed game.";
        case ImportStatus::kCancelled:
            return "The check was cancelled.";
        case ImportStatus::kBadManifest:
            return "The install manifest cannot be used (" + manifestError +
                   "), so the imported game cannot be checked. The client's files are "
                   "incomplete.";
        case ImportStatus::kVersion2500:
            return path + " is version 2.5.0.0, not 2.5.5.2 as imported. Import the game again "
                          "from an install patched to 2.5.5.2.";
        case ImportStatus::kExeModified:
            return path + " has been changed since it was imported. Import the game again from "
                          "the installed game.";
        case ImportStatus::kNotTheGame:
            return path + " is not Starfleet Command: Orion Pirates 2.5.5.2. Import the game "
                          "again from the installed game.";
        case ImportStatus::kTooManyFolders:
            return "The imported game in " + gameDir + " holds more than " +
                   std::to_string(kMaxFolders) + " folders, or folders more than " +
                   std::to_string(kMaxInstallDepth) +
                   " deep, so it was not all looked at. Import the game again from the "
                   "installed game.";
        default:
            break; // a check reports no other status (kOwnCopy is an import's alone)
        }
    }
    switch (status) {
    case ImportStatus::kVerified:
        return "The imported game in " + gameDir + " was checked."; // a check answers above
    case ImportStatus::kImported:
        text = "Imported " + std::to_string(filesCopied) + " files (" + megabytes(bytesCopied) +
               ") from " + installDir + " into " + gameDir + ".";
        if (!changed.empty()) {
            text += "\nThese files differ from the 2.5.5.2 release and were imported as they are:";
            for (const std::string &path : changed) {
                text += "\n  " + path;
            }
        }
        if (!modScripts.empty()) {
            text += "\nThese mission scripts are not part of 2.5.5.2 and were imported as mod "
                    "scripts:";
            for (const std::string &path : modScripts) {
                text += "\n  mod script: " + path;
            }
        }
        return text;
    case ImportStatus::kNotFound:
        return "No " + std::string(kGameExe) + " was found in " + source +
               " or in the folders below it. Choose the folder Starfleet Command: Orion Pirates "
               "is installed in.";
    case ImportStatus::kDiscOrInstaller:
        return source + " is the game's disc, an image of it, an installer or an archive, not "
                        "an installed game. " + reinstall;
    case ImportStatus::kVersion2500:
        return path + " is version 2.5.0.0. Apply the official 2.5.5.2 patch to that install, "
                      "then import it again.";
    case ImportStatus::kExeModified:
        return path + " has been modified. Restore the 2.5.5.2 " + kGameExe +
               " (reinstall and apply the official 2.5.5.2 patch), then import again.";
    case ImportStatus::kNotTheGame:
        return path + " is not Starfleet Command: Orion Pirates 2.5.5.2. " + reinstall;
    case ImportStatus::kDamaged:
        text = "The install at " + installDir +
               " is damaged: these files are missing, empty or shorter than in the 2.5.5.2 "
               "release, so nothing was imported:";
        for (const DamagedFile &file : damaged) {
            text += "\n  " + file.problem + ": " + file.path;
            if (file.problem == "truncated") {
                text += " (" + std::to_string(file.size) + " of " +
                        std::to_string(file.expectedSize) + " bytes)";
            }
        }
        return text + "\nCopy these files from another install of 2.5.5.2, or reinstall and "
                      "apply the official 2.5.5.2 patch, then import again.";
    case ImportStatus::kNoSpace:
        return "Importing needs " + megabytes(bytesNeeded) + " free at " + path + ", and " +
               megabytes(bytesFree) + " are free. Free some space, then import again.";
    case ImportStatus::kCancelled:
        return "The import was cancelled; nothing was kept.";
    case ImportStatus::kWriteFailed:
        if (!earlierAt.empty()) {
            return "The import could not write " + path + ". The earlier import is in " +
                   earlierAt + "; the next import puts it back first. Check that the folder is "
                               "writable, then import again.";
        }
        return "The import could not write " + path + "; nothing was kept. Check that the "
                                                      "folder is writable, then import again.";
    case ImportStatus::kOwnCopy:
        if (!ownCopyLink.empty()) {
            return ownCopyLink + " in the install at " + installDir +
                   " links into the client's own copy of the game, or to a folder holding it, "
                   "and the import replaces that copy, so nothing was imported. Remove the link, then import again.";
        }
        return source + " is the client's own copy of the game, or a folder of it, not an "
                        "installed game. Choose the folder Starfleet Command: Orion Pirates is "
                        "installed in.";
    case ImportStatus::kTooManyFolders:
        return "The install at " + installDir + " holds more than " +
               std::to_string(kMaxFolders) + " folders, or folders more than " +
               std::to_string(kMaxInstallDepth) +
               " deep, so it was not all looked at and nothing was imported. "
               "Choose the folder Starfleet Command: Orion Pirates is installed in.";
    case ImportStatus::kBadManifest:
        return "The install manifest cannot be used (" + manifestError +
               "), so nothing can be imported. The client's files are incomplete.";
    }
    return text;
}

ImportReport importGame(const ImportRequest &request, ImportFiles &files,
                        const ImportCallbacks &callbacks) {
    return Importer(request, files, callbacks).run();
}

ImportReport verifyImportedGame(const std::string &gameDir, const InstallManifest &manifest,
                                ImportFiles &files, const ImportCallbacks &callbacks) {
    ImportRequest request;
    request.source = gameDir;
    request.gameDir = gameDir;
    request.manifest = manifest;
    return Importer(request, files, callbacks).verify();
}

} // namespace sfcop::app
