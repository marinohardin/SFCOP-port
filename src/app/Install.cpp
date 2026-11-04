#include "app/Install.h"

#include <map>
#include <memory>

#include "app/ExitCodes.h"
#include "app/Sha256.h"

namespace sfcop::app {

namespace {

constexpr std::size_t kHexDigits = 64;
constexpr std::size_t kReadPiece = 1 << 20;

// The installer's company and product, from the registry key it writes; the folder it offers
// under Program Files is assumed to follow them.
constexpr const char kCompanyFolder[] = "Taldren";
constexpr const char kProductFolder[] = "Starfleet Command Orion Pirates";

bool isHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

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
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string baseName(const std::string &path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool isUnder(const std::string &path, const std::string &dir) {
    return dir.empty() || (path.size() > dir.size() + 1 && path.compare(0, dir.size(), dir) == 0 &&
                           path[dir.size()] == '/');
}

VerifyResult refuse(VerifyStatus status, const std::string &root, const std::string &path) {
    VerifyResult result;
    result.status = status;
    result.root = root;
    result.path = path;
    return result;
}

} // namespace

HashList parseHashList(const std::string &text) {
    HashList list;
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

        std::size_t digits = 0;
        while (digits < row.size() && isHex(row[digits])) {
            ++digits;
        }
        if (digits != kHexDigits) {
            list.error = lineError(line, "expected 64 hex digits of SHA-256 at the start");
            return list;
        }
        if (row.size() < kHexDigits + 3 || row[kHexDigits] != ' ' ||
            (row[kHexDigits + 1] != ' ' && row[kHexDigits + 1] != '*')) {
            list.error = lineError(line, "expected two spaces, or a space and '*', then a path");
            return list;
        }
        HashEntry entry;
        entry.line = line;
        entry.path = row.substr(kHexDigits + 2);
        if (!isInsideRoot(entry.path)) {
            list.error = lineError(line, "the path '" + entry.path +
                                             "' must be relative, with '/' between parts, and "
                                             "stay inside the root");
            return list;
        }
        const auto [previous, isNew] = firstLine.emplace(entry.path, line);
        if (!isNew) {
            list.error = lineError(line, "'" + entry.path + "' is listed twice (first on line " +
                                             std::to_string(previous->second) + ")");
            return list;
        }
        for (std::size_t i = 0; i < kHexDigits; ++i) {
            entry.sha256 += lower(row[i]);
        }
        list.entries.push_back(std::move(entry));
    }
    return list;
}

HashList installEntries(const HashList &list) {
    HashList result;
    if (!list.ok()) {
        result.error = list.error;
        return result;
    }
    std::optional<std::string> bestDir;
    std::size_t bestCount = 0;
    for (const HashEntry &exe : list.entries) {
        if (baseName(exe.path) != kGameExe) {
            continue;
        }
        const std::string dir = parentOf(exe.path);
        std::size_t count = 0;
        for (const HashEntry &entry : list.entries) {
            count += isUnder(entry.path, dir) ? 1 : 0;
        }
        if (!bestDir || count > bestCount) {
            bestDir = dir;
            bestCount = count;
        }
    }
    if (!bestDir) {
        result.error = std::string("the list names no ") + kGameExe;
        return result;
    }
    const std::size_t strip = bestDir->empty() ? 0 : bestDir->size() + 1;
    for (const HashEntry &entry : list.entries) {
        if (isUnder(entry.path, *bestDir)) {
            HashEntry inside = entry;
            inside.path = entry.path.substr(strip);
            result.entries.push_back(std::move(inside));
        }
    }
    return result;
}

int VerifyResult::exitCode() const { return accepted() ? kExitSuccess : kExitInstallRefused; }

std::string VerifyResult::message() const {
    std::string what;
    switch (status) {
    case VerifyStatus::kAccepted:
        return "The install at " + root + " is Orion Pirates 2.5.5.2.";
    case VerifyStatus::kMissing:
        what = path + " is missing from the install at " + root + ".";
        break;
    case VerifyStatus::kMismatch:
        what = path + " in the install at " + root + " differs from the 2.5.5.2 release.";
        break;
    case VerifyStatus::kUnreadable:
        what = path + " in the install at " + root + " could not be read.";
        break;
    }
    return what + " Reinstall Starfleet Command: Orion Pirates from its disc and apply the "
                  "official 2.5.5.2 patch, then start sfcop again.";
}

VerifyResult verifyInstall(const std::string &root, const std::vector<HashEntry> &entries,
                           const FileSystem &files, const ProgressFn &progress) {
    VerifyProgress state;
    state.fileCount = entries.size();
    for (const HashEntry &entry : entries) {
        if (const std::optional<std::uint64_t> size = files.fileSize(joinPath(root, entry.path))) {
            state.bytesTotal += *size;
        }
    }

    std::vector<unsigned char> piece(kReadPiece);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const HashEntry &entry = entries[i];
        const std::string full = joinPath(root, entry.path);
        if (!files.fileSize(full)) {
            return refuse(VerifyStatus::kMissing, root, entry.path);
        }
        const std::unique_ptr<FileReader> reader = files.openRead(full);
        if (!reader) {
            return refuse(VerifyStatus::kUnreadable, root, entry.path);
        }
        state.fileIndex = i;
        state.path = entry.path;
        Sha256 hash;
        while (const std::size_t got = reader->read(piece.data(), piece.size())) {
            hash.update(piece.data(), got);
            state.bytesDone += got;
            if (progress) {
                progress(state);
            }
        }
        if (reader->failed()) {
            return refuse(VerifyStatus::kUnreadable, root, entry.path);
        }
        if (Sha256::toHex(hash.finish()) != entry.sha256) {
            return refuse(VerifyStatus::kMismatch, root, entry.path);
        }
    }
    VerifyResult accepted;
    accepted.root = root;
    return accepted;
}

bool holdsInstall(const std::string &dir, const FileSystem &files) {
    return !dir.empty() && files.fileSize(joinPath(dir, kGameExe)).has_value();
}

LocateResult locateInstall(const LocateRequest &request, const FileSystem &files) {
    LocateResult result;
    const auto found = [&result](const std::string &dir, InstallSource source) {
        result.dir = dir;
        result.source = source;
        return result;
    };
    if (request.flag) {
        if (holdsInstall(*request.flag, files)) {
            return found(*request.flag, InstallSource::kFlag);
        }
        result.error = "--install " + *request.flag + " holds no " + kGameExe + ".";
        return result;
    }
    if (request.remembered && holdsInstall(*request.remembered, files)) {
        return found(*request.remembered, InstallSource::kRemembered);
    }
    for (const std::string &dir : request.defaults) {
        if (holdsInstall(dir, files)) {
            return found(dir, InstallSource::kDefault);
        }
    }
    if (request.ask) {
        if (const std::optional<std::string> answer = request.ask()) {
            if (holdsInstall(*answer, files)) {
                return found(*answer, InstallSource::kAsked);
            }
            result.error = *answer + " holds no " + kGameExe + ".";
            return result;
        }
    }
    result.error = std::string("No install of Starfleet Command: Orion Pirates was found. Give "
                               "its folder, the one that holds ") +
                   kGameExe + ", with --install DIR.";
    return result;
}

std::vector<std::string> defaultInstallDirs(Platform platform, const EnvLookup &env) {
    std::vector<std::string> dirs;
    const auto add = [&dirs](const std::string &dir) {
        for (const std::string &known : dirs) {
            if (known == dir) {
                return;
            }
        }
        dirs.push_back(dir);
    };
    const auto value = [&env](const char *name) -> std::optional<std::string> {
        std::optional<std::string> v = env(name);
        if (v && v->empty()) {
            return std::nullopt;
        }
        return v;
    };

    if (platform == Platform::kWindows) {
        for (const char *name : {"ProgramFiles(x86)", "ProgramFiles"}) {
            if (const std::optional<std::string> base = value(name)) {
                std::string dir = *base;
                if (dir.back() != '\\' && dir.back() != '/') {
                    dir += '\\';
                }
                add(dir + kCompanyFolder + "\\" + kProductFolder);
            }
        }
        return dirs;
    }

    std::optional<std::string> prefix = value("WINEPREFIX");
    if (!prefix) {
        if (const std::optional<std::string> home = value("HOME")) {
            prefix = joinPath(*home, ".wine");
        }
    }
    if (prefix) {
        for (const char *programs : {"Program Files (x86)", "Program Files"}) {
            add(joinPath(*prefix, std::string("drive_c/") + programs + "/" + kCompanyFolder + "/" +
                                      kProductFolder));
        }
    }
    return dirs;
}

} // namespace sfcop::app
