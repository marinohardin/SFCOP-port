#include "app/Startup.h"

#include <iostream>

#include "app/DataDir.h"
#include "app/ExitCodes.h"

namespace sfcop::app {

namespace {

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
    request.remembered = rememberedInstall(dataDir, files);
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

    const VerifyResult verified =
        verifyInstall(installDir, entries.entries, files, config.progress);
    if (!verified.accepted()) {
        err << "sfcop: " << verified.message() << "\n";
        return stop(verified.exitCode());
    }
    if (request.remembered != installDir && !rememberInstall(dataDir, installDir, files)) {
        err << "sfcop: the install's path could not be saved in " << dataDir
            << "; the next run will look for it again.\n";
    }

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
