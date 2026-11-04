// The checks of ModuleContract.md §5 that do not depend on the platform: every ModuleLoader makes
// them through checkImage(), so the mock and the real loader refuse the same modules.
#include "app/scripts/ModuleLoader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace sfcop::scripts {

namespace {

constexpr char kMagic[8] = {'S', 'F', 'C', 'O', 'P', 'M', 'O', 'D'};

std::string versionText(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f", static_cast<double>(v));
    return buf;
}

// The tag's ABI string up to its terminator, or all 32 bytes if it has none.
std::string_view abiText(const ModuleAbiTag &tag) {
    const char *end =
        static_cast<const char *>(std::memchr(tag.compilerAbi, '\0', sizeof tag.compilerAbi));
    return std::string_view(tag.compilerAbi, end ? static_cast<std::size_t>(end - tag.compilerAbi)
                                                 : sizeof tag.compilerAbi);
}

// Bytes from the module's file, fit for a message: printable ASCII as is, anything else as \xHH.
std::string printable(std::string_view bytes) {
    std::string out;
    for (const char c : bytes) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u < 0x7f && u != '\\') {
            out += c;
        } else {
            char buf[5];
            std::snprintf(buf, sizeof buf, "\\x%02X", u);
            out += buf;
        }
    }
    return out;
}

std::optional<std::string> abiMismatch(const ModuleAbiTag &tag) {
    if (std::memcmp(tag.magic, kMagic, sizeof kMagic) != 0) {
        return "magic " + printable(std::string_view(tag.magic, sizeof tag.magic)) +
               ", expected " + std::string(kMagic, sizeof kMagic);
    }
    if (tag.contractVersion != kModuleContractVersion) {
        return "contract version " + std::to_string(tag.contractVersion) + ", expected " +
               std::to_string(kModuleContractVersion);
    }
    if (tag.scriptVersion != kScriptVersion) {
        return "scripting library version " + versionText(tag.scriptVersion) + ", expected " +
               versionText(kScriptVersion);
    }
    if (abiText(tag) != kHostAbiTag) {
        return "compiler ABI " + printable(abiText(tag)) + ", expected " +
               std::string(kHostAbiTag);
    }
    return std::nullopt;
}

LoadError refuse(Refusal reason, std::string_view module, std::string detail = {}) {
    return LoadError{reason, std::string(module), std::move(detail)};
}

bool exports(const ModuleImage &image, std::string_view name) {
    return std::find(image.exportedSymbols.begin(), image.exportedSymbols.end(), name) !=
           image.exportedSymbols.end();
}

} // namespace

ModuleAbiTag hostAbiTag() {
    ModuleAbiTag tag{};
    std::memcpy(tag.magic, kMagic, sizeof kMagic);
    tag.contractVersion = kModuleContractVersion;
    tag.scriptVersion = kScriptVersion;
    std::memcpy(tag.compilerAbi, kHostAbiTag.data(),
                std::min(kHostAbiTag.size(), sizeof tag.compilerAbi - 1));
    return tag;
}

std::string describe(const LoadError &error) {
    const std::string head = "refused: " + error.module + ": ";
    switch (error.reason) {
    case Refusal::kNotFound:
        return head + "not found";
    case Refusal::kNoManifest:
        return head + "no module.json beside the library";
    case Refusal::kBadManifest:
        return head + "module.json is invalid: " + error.detail;
    case Refusal::kNotLoadable:
        return head + "not a loadable module for this host: " + error.detail;
    case Refusal::kNoAbiTag:
        return head + "no sfcop_module_abi version tag";
    case Refusal::kAbiMismatch:
        return head + "version tag mismatch: " + error.detail;
    case Refusal::kForeignImport:
        return head + "imports " + error.detail +
               ", which is outside the scripting library and the C runtime";
    case Refusal::kNoEntryPoint:
        return head + "does not export " + error.detail;
    }
    return head + "refused";
}

AllowList AllowList::parse(std::string_view text) {
    AllowList list;
    while (!text.empty()) {
        const std::size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view() : text.substr(eol + 1);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (!line.empty() && line.front() != '#') {
            list.symbols_.emplace_back(line);
        }
    }
    std::sort(list.symbols_.begin(), list.symbols_.end());
    list.symbols_.erase(std::unique(list.symbols_.begin(), list.symbols_.end()),
                        list.symbols_.end());
    return list;
}

bool AllowList::contains(std::string_view symbol) const {
    return std::binary_search(symbols_.begin(), symbols_.end(), symbol);
}

bool AllowList::namesModule(std::string_view dll) const {
    if (dll.empty()) {
        return false;
    }
    const std::string prefix = std::string(dll) + "!";
    auto it = std::lower_bound(symbols_.begin(), symbols_.end(), prefix);
    return it != symbols_.end() && it->compare(0, prefix.size(), prefix) == 0;
}

std::string normalizeModuleName(std::string_view name) {
    const std::size_t slash = name.find_last_of("\\/");
    if (slash != std::string_view::npos) {
        name.remove_prefix(slash + 1);
    }
    std::string out(name);
    for (char &c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    if (!out.empty() && out.find('.') == std::string::npos) {
        out += ".DLL";
    }
    return out;
}

std::optional<LoadError> checkDynamicLookup(std::string_view module, std::string_view dll,
                                            std::string_view symbol, const AllowList &allowList) {
    const std::string name = normalizeModuleName(dll);
    if (!allowList.namesModule(name)) {
        return refuse(Refusal::kForeignImport, module,
                      symbol.empty() ? name : name + "!" + std::string(symbol));
    }
    if (!symbol.empty() && !allowList.contains(name + "!" + std::string(symbol))) {
        return refuse(Refusal::kForeignImport, module, name + "!" + std::string(symbol));
    }
    return std::nullopt;
}

std::optional<LoadError> checkImports(std::string_view module,
                                      const std::vector<std::string> &imports,
                                      const AllowList &allowList) {
    for (const std::string &name : imports) {
        if (!allowList.contains(name)) {
            return refuse(Refusal::kForeignImport, module, name);
        }
    }
    return std::nullopt;
}

std::optional<LoadError> checkImage(std::string_view module, const ModuleImage &image,
                                    const AllowList &allowList, const AllowList &hostRuntime) {
    if (!image.manifest && image.manifestError.empty()) {
        return refuse(Refusal::kNoManifest, module);
    }
    if (!image.manifestError.empty()) {
        return refuse(Refusal::kBadManifest, module, image.manifestError);
    }
    if (image.manifest->name != module) {
        return refuse(Refusal::kBadManifest, module,
                      "name " + image.manifest->name + ", expected " + std::string(module));
    }
    if (!image.notLoadable.empty()) {
        return refuse(Refusal::kNotLoadable, module, image.notLoadable);
    }
    if (!image.abiTag) {
        return refuse(Refusal::kNoAbiTag, module);
    }
    if (std::optional<std::string> why = abiMismatch(*image.abiTag)) {
        return refuse(Refusal::kAbiMismatch, module, std::move(*why));
    }
    for (const std::string &symbol : image.undefinedSymbols) {
        // A library import is named by the original's name, the prefix dropped, as gen_allowlist.py
        // --check names it.
        std::string_view name = symbol;
        const bool library = name.substr(0, kImportPrefix.size()) == kImportPrefix;
        if (library) {
            name.remove_prefix(kImportPrefix.size());
        }
        if (!(library ? allowList.contains(name) : hostRuntime.contains(name))) {
            return refuse(Refusal::kForeignImport, module, std::string(name));
        }
    }
    for (std::string_view entry : {kMissionSpecEntryName, kScriptEntryName}) {
        if (!exports(image, entry)) {
            return refuse(Refusal::kNoEntryPoint, module, std::string(entry));
        }
    }
    return std::nullopt;
}

} // namespace sfcop::scripts
