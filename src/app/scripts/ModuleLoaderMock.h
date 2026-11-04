// A ModuleLoader over modules held in memory, for tests and for code that loads missions before the
// real loader exists. It makes the same checks as the real loader (checkImage), on images a test
// describes instead of files, and hands out the entry points the test supplies.
#pragma once

#include "app/scripts/ModuleLoader.h"

#include <algorithm>
#include <map>
#include <utility>

namespace sfcop::scripts {

class ModuleLoaderMock final : public ModuleLoader {
public:
    ModuleLoaderMock(AllowList allowList, AllowList hostRuntime)
        : allowList_(std::move(allowList)), hostRuntime_(std::move(hostRuntime)) {}

    // Puts a module at `library`, as scr2native would have written it.
    void addModule(const std::filesystem::path &library, ModuleImage image,
                   MissionSpecEntryFn missionSpecEntry, ScriptEntryFn scriptEntry) {
        modules_[library] = Stored{std::move(image), missionSpecEntry, scriptEntry, 0};
    }

    std::optional<LoadError> verify(const std::filesystem::path &library) override {
        const std::string name = library.stem().string();
        auto it = modules_.find(library);
        if (it == modules_.end()) {
            return LoadError{Refusal::kNotFound, name, {}};
        }
        return checkImage(name, it->second.image, allowList_, hostRuntime_);
    }

    LoadResult load(const std::filesystem::path &library) override {
        if (std::optional<LoadError> error = verify(library)) {
            return LoadResult{{}, std::move(error)};
        }
        Stored &m = modules_.at(library);
        if (m.id == 0) {
            m.id = nextId_++;
        }
        return LoadResult{LoadedModule{m.id, *m.image.manifest, m.missionSpecEntry, m.scriptEntry},
                          std::nullopt};
    }

    void unload(ModuleId id) override {
        for (auto &entry : modules_) {
            if (id != 0 && entry.second.id == id) {
                entry.second.id = 0;
            }
        }
    }

    std::optional<LoadError> checkLookup(ModuleId id, std::string_view dll,
                                         std::string_view symbol) override {
        for (auto &entry : modules_) {
            if (id == 0 || entry.second.id != id) {
                continue;
            }
            const std::string name = entry.first.stem().string();
            if (!dll.empty()) {
                return checkDynamicLookup(name, dll, symbol, allowList_);
            }
            const std::vector<std::string> &own = entry.second.image.exportedSymbols;
            if (symbol.empty() || std::find(own.begin(), own.end(), symbol) != own.end()) {
                return std::nullopt;
            }
            return LoadError{Refusal::kForeignImport, name, std::string(symbol)};
        }
        return LoadError{Refusal::kNotFound, "module " + std::to_string(id), {}};
    }

    bool isLoaded(const std::filesystem::path &library) const {
        auto it = modules_.find(library);
        return it != modules_.end() && it->second.id != 0;
    }

private:
    struct Stored {
        ModuleImage image;
        MissionSpecEntryFn missionSpecEntry = nullptr;
        ScriptEntryFn scriptEntry = nullptr;
        ModuleId id = 0;
    };

    AllowList allowList_;
    AllowList hostRuntime_;
    std::map<std::filesystem::path, Stored> modules_;
    ModuleId nextId_ = 1;
};

} // namespace sfcop::scripts
