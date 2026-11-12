#include "app/DataDir.h"

namespace sfcop::app {

namespace {

constexpr const char kFolder[] = "SFCOP";

std::optional<std::string> nonEmpty(const EnvLookup &env, const char *name) {
    std::optional<std::string> value = env(name);
    if (value && value->empty()) {
        return std::nullopt;
    }
    return value;
}

DataDirResult found(std::string dir) {
    DataDirResult result;
    result.dir = std::move(dir);
    return result;
}

DataDirResult unset(const char *variable) {
    DataDirResult result;
    result.error = std::string("The per-user data folder cannot be found because ") + variable +
                   " is not set. Give one with --data DIR.";
    return result;
}

// The first line of a small file in the data folder; nothing when it is missing or that line is
// empty.
std::optional<std::string> firstLineOf(const std::string &path, const FileSystem &files) {
    std::optional<std::string> text = files.readText(path);
    if (!text) {
        return std::nullopt;
    }
    const std::size_t end = text->find_first_of("\r\n");
    if (end != std::string::npos) {
        text->resize(end);
    }
    if (text->empty()) {
        return std::nullopt;
    }
    return text;
}

bool isAbsolutePath(const std::string &path) {
    return (!path.empty() && (path[0] == '/' || path[0] == '\\')) ||
           (path.size() > 1 && path[1] == ':');
}

} // namespace

DataDirResult resolveDataDir(Platform platform, const EnvLookup &env,
                             const std::optional<std::string> &flag) {
    if (flag) {
        return found(*flag);
    }
    switch (platform) {
    case Platform::kWindows:
        if (const std::optional<std::string> appData = nonEmpty(env, "APPDATA")) {
            return found(joinPath(*appData, kFolder));
        }
        return unset("APPDATA");
    case Platform::kLinux:
        if (const std::optional<std::string> xdg = nonEmpty(env, "XDG_DATA_HOME")) {
            if ((*xdg)[0] == '/') {
                return found(joinPath(*xdg, kFolder));
            }
        }
        if (const std::optional<std::string> home = nonEmpty(env, "HOME")) {
            return found(joinPath(*home, std::string(".local/share/") + kFolder));
        }
        return unset("HOME");
    case Platform::kMacos:
        break;
    }
    if (const std::optional<std::string> home = nonEmpty(env, "HOME")) {
        return found(joinPath(*home, std::string("Library/Application Support/") + kFolder));
    }
    return unset("HOME");
}

std::string ensureDataDir(const std::string &dir, FileSystem &files) {
    if (!files.createDirectories(dir)) {
        return "The per-user data folder " + dir + " cannot be created.";
    }
    for (const char *sub : kDataSubdirs) {
        const std::string path = joinPath(dir, sub);
        if (!files.createDirectories(path)) {
            return "The folder " + path + " cannot be created.";
        }
    }
    return std::string();
}

std::string gameDirFor(Platform platform, const EnvLookup &env, bool dataFlag,
                       const std::string &dataDir) {
    if (platform == Platform::kWindows && !dataFlag) {
        if (const std::optional<std::string> local = nonEmpty(env, "LOCALAPPDATA")) {
            return joinPath(joinPath(*local, kFolder), kImportedGame);
        }
    }
    return joinPath(dataDir, kImportedGame);
}

std::optional<std::string> rememberedInstall(const std::string &dataDir, const FileSystem &files) {
    return firstLineOf(joinPath(dataDir, kRememberedInstallFile), files);
}

std::optional<std::string> rememberedInstallDir(const std::string &dataDir,
                                                const std::string &gameDir,
                                                const FileSystem &files) {
    std::optional<std::string> dir = rememberedInstall(dataDir, files);
    if (!dir || isAbsolutePath(*dir)) {
        return dir;
    }
    if (*dir == kImportedGame) {
        return gameDir;
    }
    return joinPath(dataDir, *dir);
}

bool rememberInstall(const std::string &dataDir, const std::string &installDir, FileSystem &files) {
    return files.writeTextAtomic(joinPath(dataDir, kRememberedInstallFile), installDir + "\n");
}

std::optional<std::string> rememberedMissionModules(const std::string &dataDir,
                                                    const FileSystem &files) {
    std::optional<std::string> dir =
        firstLineOf(joinPath(dataDir, kRememberedMissionModulesFile), files);
    if (!dir || isAbsolutePath(*dir)) {
        return dir;
    }
    return joinPath(dataDir, *dir);
}

} // namespace sfcop::app
