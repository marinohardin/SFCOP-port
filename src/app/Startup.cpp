#include "app/Startup.h"

#include <chrono>
#include <iostream>

#include "app/DataDir.h"
#include "app/ExitCodes.h"
#include "app/Import.h"
#include "app/ImportCommand.h"
#include "fixes/fixes.h"

namespace sfcop::app {

namespace {

bool sameIgnoringCase(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto fold = [](char c) {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        };
        if (fold(a[i]) != fold(b[i])) {
            return false;
        }
    }
    return true;
}

Startup stop(int code) {
    Startup result;
    result.exitCode = code;
    return result;
}

std::string trimmed(const std::string &text) {
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

void printFixProblems(const std::vector<fixes::Problem> &problems, std::ostream &err) {
    for (const fixes::Problem &problem : problems) {
        if (problem.source == "--fix") {
            err << "sfcop: --fix: " << problem.message << "; it is ignored.\n";
        } else {
            err << "sfcop: " << problem.source << ", line " << problem.line << ": "
                << problem.message << "; it is ignored.\n";
        }
    }
}

// The fix switches' positions (src/fixes/README.md): the defaults, then settings/fixes.gf in the
// data folder when it exists, then each --fix. Each problem is printed once and stops nothing.
fixes::Settings fixSettings(const ClientOptions &options, const std::string &dataDir,
                            const FileSystem &files, std::ostream &err) {
    fixes::Settings settings;
    const std::string path = joinPath(dataDir, "settings/fixes.gf");
    if (files.fileSize(path)) {
        if (const std::optional<std::string> text = files.readText(path)) {
            printFixProblems(settings.applyFile(*text, path), err);
        } else {
            err << "sfcop: " << path << " cannot be read; the fixes keep their defaults.\n";
        }
    }
    std::vector<std::string> values;
    for (const FixSetting &fix : options.fixes) {
        values.push_back(fix.name + (fix.on ? "=on" : "=off"));
    }
    printFixProblems(settings.applyCommandLine(values), err);
    return settings;
}

} // namespace

Startup startClient(int argc, const char *const *argv, const StartupConfig &config) {
    std::ostream &out = config.out != nullptr ? *config.out : std::cout;
    std::ostream &err = config.err != nullptr ? *config.err : std::cerr;
    FileSystem &files = config.files != nullptr ? *config.files : hostFileSystem();

    const ParseResult parsed = parseCommandLine(argc, argv, config.extraFlags);
    if (parsed.outcome == ParseOutcome::kHelp) {
        out << helpText(config.extraFlags);
        return stop(kExitSuccess);
    }
    if (parsed.outcome == ParseOutcome::kError) {
        err << "sfcop: " << parsed.error << "\nRun sfcop --help to see the flags.\n";
        return stop(kExitBadInput);
    }
    const ClientOptions &options = parsed.options;

    const DataDirResult data = resolveDataDir(config.platform, config.env, options.dataDir);
    if (!data.dir) {
        err << "sfcop: " << data.error << "\n";
        return stop(kExitBadInput);
    }
    // Absolute, so that what is remembered holds from any folder the next run starts in.
    const std::string dataDir = files.absolutePath(*data.dir);
    if (const std::string problem = ensureDataDir(dataDir, files); !problem.empty()) {
        err << "sfcop: " << problem << "\n";
        return stop(kExitFailure);
    }
    const std::string gameDir =
        gameDirFor(config.platform, config.env, options.dataDir.has_value(), dataDir);

    if (options.importFrom) {
        ImportCommand command;
        command.source = *options.importFrom;
        command.dataDir = dataDir;
        command.gameDir = gameDir;
        command.manifestPath = installManifestBeside(config.hashListPath);
        command.seconds = config.seconds;
        ImportFiles &importFiles =
            config.importFiles != nullptr ? *config.importFiles : hostImportFiles();
        return stop(runImportCommand(command, files, importFiles, out, err));
    }

    const std::optional<std::string> listText = files.readText(config.hashListPath);
    if (!listText) {
        err << "sfcop: the hash list " << config.hashListPath
            << " cannot be read, so the install cannot be verified.\n";
        return stop(kExitInstallRefused);
    }
    const HashList entries = installEntries(parseHashList(*listText));
    if (!entries.ok()) {
        err << "sfcop: the hash list " << config.hashListPath << ", " << entries.error
            << ", so the install cannot be verified.\n";
        return stop(kExitInstallRefused);
    }

    LocateRequest request;
    if (options.installDir) {
        request.flag = files.absolutePath(*options.installDir);
    }
    request.remembered = rememberedInstallDir(dataDir, gameDir, files);
    request.defaults = config.installDirs;
    for (const std::string &dir : defaultInstallDirs(config.platform, config.env)) {
        request.defaults.push_back(dir);
    }
    if (!options.headless) {
        request.ask = config.ask;
    }
    const LocateResult located = locateInstall(request, files);
    if (!located.dir) {
        err << "sfcop: " << located.error << "\n";
        return stop(kExitInstallRefused);
    }
    const std::string installDir = files.absolutePath(*located.dir);

    // The client's own imported copy is accepted as its import accepted it: the files its record
    // lists as changed or unchecked may differ from the hash list (Import.h, ImportRecord). A
    // record in any other folder is not ours and is ignored, and the executable is always checked,
    // as the import checks it. The record is a convenience, not an integrity check: whoever can
    // write it can as well replace the game in the same folder.
    const std::string recordPath = joinPath(installDir, kImportRecordFile);
    const std::optional<std::string> recordText =
        installDir == files.absolutePath(gameDir) ? files.readText(recordPath) : std::nullopt;
    const ImportRecord record = recordText ? parseImportRecord(*recordText) : ImportRecord{};
    std::vector<std::string> mayDiffer;
    for (const std::vector<std::string> *list : {&record.changed, &record.unchecked}) {
        for (const std::string &path : *list) {
            if (!sameIgnoringCase(path, kGameExe)) {
                mayDiffer.push_back(path);
            }
        }
    }
    const VerifyResult verified = verifyInstall(installDir, runtimeEntries(entries.entries), files,
                                                config.progress, mayDiffer);
    if (!verified.accepted()) {
        err << "sfcop: " << verified.message() << "\n";
        return stop(verified.exitCode());
    }
    std::size_t changedInUse = 0;
    for (const std::string &path : verified.differing) {
        for (const std::string &changed : record.changed) {
            if (sameIgnoringCase(path, changed)) {
                ++changedInUse;
                break;
            }
        }
    }
    if (changedInUse > 0) {
        const bool one = changedInUse == 1;
        err << "sfcop: " << changedInUse << (one ? " file" : " files") << " of the imported game "
            << (one ? "differs" : "differ") << " from the 2.5.5.2 release and "
            << (one ? "is" : "are") << " used as imported; " << recordPath << " lists "
            << (one ? "it" : "them") << ".\n";
    }
    // A one-off --install does not replace the link to an imported copy that is still there.
    const bool keepImport = options.installDir && request.remembered == gameDir &&
                            holdsInstall(gameDir, files);
    if (request.remembered != installDir && !keepImport &&
        !rememberInstall(dataDir, installDir, files)) {
        err << "sfcop: the install's path could not be saved in " << dataDir
            << "; the next run will look for it again.\n";
    }
    // Before the game's threads start, which is after this returns.
    fixes::install(fixSettings(options, dataDir, files, err));

    Startup result;
    result.options = options;
    result.installDir = installDir;
    result.dataDir = dataDir;
    return result;
}

StartupConfig consoleStartup(std::string hashListPath, std::vector<ExtraFlag> extraFlags,
                             std::vector<std::string> installDirs) {
    StartupConfig config;
    config.hashListPath = std::move(hashListPath);
    config.extraFlags = std::move(extraFlags);
    config.installDirs = std::move(installDirs);
    config.ask = askOnConsole(std::cin, std::cout);
    config.seconds = [] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    return config;
}

std::function<std::optional<std::string>()> askOnConsole(std::istream &in, std::ostream &out) {
    return [&in, &out]() -> std::optional<std::string> {
        out << "Where is Starfleet Command: Orion Pirates installed? Give the folder that holds "
            << kGameExe << ": " << std::flush;
        std::string line;
        if (!std::getline(in, line)) {
            return std::nullopt;
        }
        std::string answer = trimmed(line);
        if (answer.empty()) {
            return std::nullopt;
        }
        return answer;
    };
}

} // namespace sfcop::app
