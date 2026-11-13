#include "app/launcher/LauncherSetup.h"

#include <algorithm>

#include "app/DataDir.h"
#include "app/Import.h"
#include "app/Install.h"

namespace sfcop::app::launcher {

std::vector<Suggestion> suggestionsFrom(const std::vector<std::string> &dirs,
                                        const FileSystem &files) {
    std::vector<Suggestion> suggestions;
    for (const std::string &dir : dirs) {
        if (dir.empty() || !files.isDirectory(dir)) {
            continue;
        }
        const bool seen = std::any_of(suggestions.begin(), suggestions.end(),
                                      [&dir](const Suggestion &one) { return one.path == dir; });
        if (!seen) {
            suggestions.push_back(Suggestion{dir, holdsInstall(dir, files)});
        }
    }
    return suggestions;
}

std::optional<GameInUse> gameInUseOf(const std::string &dataDir, const std::string &gameDir,
                                     const FileSystem &files,
                                     const std::function<std::string(const std::string &)>
                                         &modifiedTime) {
    const std::optional<std::string> dir = rememberedInstallDir(dataDir, gameDir, files);
    if (!dir || !holdsInstall(*dir, files)) {
        return std::nullopt;
    }
    GameInUse game;
    game.dir = *dir;
    game.imported = files.absolutePath(*dir) == files.absolutePath(gameDir);
    if (game.imported) {
        const std::string recordPath = joinPath(gameDir, kImportRecordFile);
        if (const std::optional<std::string> text = files.readText(recordPath)) {
            const ImportRecord record = parseImportRecord(*text);
            game.source = record.source;
            game.files = record.files;
            game.bytes = record.bytes;
            if (modifiedTime) {
                game.when = modifiedTime(recordPath);
            }
        }
    }
    return game;
}

FolderPicker folderPickerOn(Platform platform, const EnvLookup &env,
                            const std::function<bool(const std::string &)> &onPath) {
    if (platform != Platform::kLinux) {
        return FolderPicker::kAvailable;
    }
    const std::optional<std::string> bus = env ? env("DBUS_SESSION_BUS_ADDRESS") : std::nullopt;
    if ((bus && !bus->empty()) || (onPath && onPath("zenity"))) {
        return FolderPicker::kAvailable;
    }
    return FolderPicker::kUnavailable;
}

Setup setupFor(const LauncherRequest &request, FolderPicker picker) {
    FileSystem &files = request.files != nullptr ? *request.files : hostFileSystem();
    Setup setup;
    setup.dataDir = request.dataDir;
    setup.gameDir = request.gameDir;
    setup.rememberedFile = kRememberedInstallFile;
    const std::string manifestPath = installManifestBeside(request.hashListPath);
    if (const std::optional<std::string> text = files.readText(manifestPath)) {
        setup.manifest = parseInstallManifest(*text);
    } else {
        setup.manifest.error = manifestPath + " cannot be read";
    }
    setup.suggestions = suggestionsFrom(request.installDirs, files);
    setup.picker = picker;
    setup.openOnReady = request.onDemand;
    return setup;
}

} // namespace sfcop::app::launcher
