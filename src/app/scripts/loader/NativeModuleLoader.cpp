// The real ModuleLoader: checkImage() on the files, then the platform's loader (ModuleContract.md
// §5, §6). Threading: one loader is used from one thread at a time (the guest machine's holder).
#include "app/scripts/loader/NativeModuleLoader.h"

#include "app/scripts/loader/OpenAhead.h"

#include <algorithm>
#include <map>
#include <system_error>
#include <utility>

namespace sfcop::scripts {

namespace {

// The lists beside ModuleLoader.h, embedded where the compiler has C23's #embed (clang accepts it
// in C++ too). A host whose baseline list has not been generated yet embeds an empty one, and a
// compiler without #embed embeds neither: every import is then refused.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
const char kAllowListText[] = {
#embed "../allowlist.txt"
    , '\0'};
#if defined(__APPLE__) && defined(__aarch64__)
const char kBaselineText[] = {
#embed "../baseline-arm64-macos-itanium.txt"
    , '\0'};
#else
const char kBaselineText[] = ""; // tools/scr2native/gen_baseline.py writes it on that host
#endif
#pragma clang diagnostic pop
#else
const char kAllowListText[] = "";
const char kBaselineText[] = "";
#endif

// A library checked and, when the checks passed, opened: what load() needs from the files.
struct Opened {
    ModuleImage image;
    std::optional<LoadError> error; // the check's refusal; nothing was opened
    void *handle = nullptr;         // null with no error: the platform's loader failed, `why`
    std::string why;
};

class NativeModuleLoader final : public ModuleLoader {
public:
    NativeModuleLoader(AllowList allowList, AllowList hostRuntime,
                       const std::vector<std::filesystem::path> &openAhead)
        : allowList_(std::move(allowList)), hostRuntime_(std::move(hostRuntime)) {
        if (!openAhead.empty()) {
            std::vector<std::string> keys;
            for (const std::filesystem::path &library : openAhead) {
                keys.push_back(keyOf(library));
            }
            // The key is the library's canonical path; opening it is what load() does first.
            ahead_ = std::make_unique<OpenAhead<Opened>>(
                std::move(keys), [this](const std::string &key) { return open(key); },
                [](Opened &opened) {
                    if (opened.handle != nullptr) {
                        native::close(opened.handle);
                    }
                });
        }
    }

    ~NativeModuleLoader() override {
        ahead_.reset(); // stops its thread and closes what was opened and never loaded
        for (auto &entry : loaded_) {
            native::close(entry.second.handle);
        }
    }

    std::optional<LoadError> verify(const std::filesystem::path &library) override {
        ModuleImage image;
        return verifyInto(library, image);
    }

    LoadResult load(const std::filesystem::path &library) override {
        const std::string key = keyOf(library);
        for (auto &entry : loaded_) {
            if (entry.second.key == key) {
                return LoadResult{entry.second.module, std::nullopt};
            }
        }
        std::optional<Opened> opened = ahead_ ? ahead_->Take(key) : std::nullopt;
        if (!opened) {
            opened = open(library);
        }
        if (opened->error) {
            return LoadResult{{}, std::move(opened->error)};
        }
        const std::string name = library.stem().string();
        void *handle = opened->handle;
        if (handle == nullptr) {
            return LoadResult{{}, LoadError{Refusal::kNotLoadable, name, loaderFailureReason(library, opened->why)}};
        }
        ModuleImage &image = opened->image;
        LoadedModule module;
        module.manifest = *image.manifest;
        module.missionSpecEntry = reinterpret_cast<MissionSpecEntryFn>(
            native::symbol(handle, std::string(kMissionSpecEntryName).c_str()));
        module.scriptEntry =
            reinterpret_cast<ScriptEntryFn>(native::symbol(handle, std::string(kScriptEntryName).c_str()));
        module.guest = static_cast<const ::sfcop_module_guest *>(
            native::symbol(handle, std::string(kGuestImageName).c_str()));
        if (module.missionSpecEntry == nullptr || module.scriptEntry == nullptr) {
            native::close(handle);
            return LoadResult{{}, LoadError{Refusal::kNoEntryPoint, name,
                                            std::string(module.missionSpecEntry == nullptr
                                                            ? kMissionSpecEntryName
                                                            : kScriptEntryName)}};
        }
        module.id = nextId_++;
        loaded_[module.id] = Loaded{key, name, handle, module, std::move(image.exportedSymbols)};
        return LoadResult{module, std::nullopt};
    }

    void unload(ModuleId id) override {
        const auto found = loaded_.find(id);
        if (found != loaded_.end()) {
            native::close(found->second.handle);
            loaded_.erase(found);
        }
    }

    std::optional<LoadError> checkLookup(ModuleId id, std::string_view dll,
                                         std::string_view symbol) override {
        const auto found = loaded_.find(id);
        if (found == loaded_.end()) {
            return LoadError{Refusal::kNotFound, "module " + std::to_string(id), {}};
        }
        if (!dll.empty()) {
            return checkDynamicLookup(found->second.name, dll, symbol, allowList_);
        }
        const std::vector<std::string> &own = found->second.exports;
        if (symbol.empty() || std::find(own.begin(), own.end(), symbol) != own.end()) {
            return std::nullopt;
        }
        return LoadError{Refusal::kForeignImport, found->second.name, std::string(symbol)};
    }

private:
    struct Loaded {
        std::string key;
        std::string name;
        void *handle;
        LoadedModule module;
        std::vector<std::string> exports;
    };

    static std::string keyOf(const std::filesystem::path &library) {
        std::error_code error;
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(library, error);
        return (error ? library : canonical).string();
    }

    // The checks of ModuleContract.md §5, then the platform's loader only if they all passed. Safe
    // on OpenAhead's thread: it reads the lists and the files, and maps the library. What it throws
    // there, OpenAhead throws again from Take() in load(), so the caller sees it as before.
    Opened open(const std::filesystem::path &library) const {
        Opened opened;
        opened.error = verifyInto(library, opened.image);
        if (!opened.error) {
            opened.handle = native::open(library, &opened.why);
        }
        return opened;
    }

    std::optional<LoadError> verifyInto(const std::filesystem::path &library, ModuleImage &image) const {
        const std::string name = library.stem().string();
        std::error_code error;
        if (!std::filesystem::is_regular_file(library, error)) {
            return LoadError{Refusal::kNotFound, name, {}};
        }
        image = readModuleImage(library);
        return checkImage(name, image, allowList_, hostRuntime_);
    }

    AllowList allowList_;
    AllowList hostRuntime_;
    std::map<ModuleId, Loaded> loaded_;
    ModuleId nextId_ = 1;
    std::unique_ptr<OpenAhead<Opened>> ahead_; // last: its thread uses the lists above
};

} // namespace

std::string loaderFailureReason(const std::filesystem::path &library, std::string_view loaderText) {
    while (!loaderText.empty() && (loaderText.back() == '\n' || loaderText.back() == '\r' ||
                                   loaderText.back() == ' ')) {
        loaderText.remove_suffix(1);
    }
    const std::size_t colon = loaderText.rfind(':');
    std::string_view message = colon == std::string_view::npos ? loaderText : loaderText.substr(colon + 1);
    while (!message.empty() && message.front() == ' ') {
        message.remove_prefix(1);
    }
    return library.filename().string() + ": " + (message.empty() ? "the library could not be loaded"
                                                                  : std::string(message));
}

const AllowList &builtInAllowList() {
    static const AllowList list = AllowList::parse(kAllowListText);
    return list;
}

const AllowList &builtInHostBaseline() {
    static const AllowList list = AllowList::parse(kBaselineText);
    return list;
}

std::unique_ptr<ModuleLoader> makeNativeModuleLoader(AllowList allowList, AllowList hostRuntime,
                                                     const std::vector<std::filesystem::path> &openAhead) {
    return std::make_unique<NativeModuleLoader>(std::move(allowList), std::move(hostRuntime), openAhead);
}

} // namespace sfcop::scripts
