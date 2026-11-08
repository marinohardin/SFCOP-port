// The provider behind LoadLibraryA, GetProcAddress and FreeLibrary for the game's mission scripts
// (GuestModules.h).
#include "app/scripts/loader/GuestModules.h"

#include "app/scripts/loader/NativeModuleLoader.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <system_error>

namespace sfcop::scripts {

namespace {

#if defined(_WIN32)
constexpr std::string_view kLibrarySuffix = ".dll";
#elif defined(__APPLE__)
constexpr std::string_view kLibrarySuffix = ".dylib";
#else
constexpr std::string_view kLibrarySuffix = ".so";
#endif
constexpr std::string_view kScriptSuffix = ".scr";

// Where scr2native may put a module (its --base range): above the guest allocator's first
// reservations, below the host's rows at 0x7F000000. 64 KB granules, as the guest's reservations.
constexpr std::uint32_t kLowestBase = 0x20000000u;
constexpr std::uint32_t kAboveModules = 0x7F000000u;
constexpr std::uint32_t kGranule = 0x10000u;

std::string_view baseName(std::string_view name) {
    const std::size_t cut = name.find_last_of("\\/");
    return cut == std::string_view::npos ? name : name.substr(cut + 1);
}

bool isScript(std::string_view name) {
    const std::string_view base = baseName(name);
    if (base.size() <= kScriptSuffix.size()) {
        return false;
    }
    const std::string_view suffix = base.substr(base.size() - kScriptSuffix.size());
    return std::equal(suffix.begin(), suffix.end(), kScriptSuffix.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == b;
    });
}

// The script's name without directory or extension: the module's name.
std::string scriptStem(std::string_view name) {
    const std::string_view base = baseName(name);
    return std::string(base.substr(0, base.size() - kScriptSuffix.size()));
}

bool sameName(const std::string &a, const std::string &b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

bool isModuleName(const std::string &name) {
    return !name.empty() && name[0] != '.' &&
           std::all_of(name.begin(), name.end(), [](char c) {
               return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-';
           });
}

void say(const char *format, const std::string &a) {
    std::fprintf(stderr, format, a.c_str());
}

// The provider the lifter's dispatch asks, and the hook it replaced.
GuestModuleProvider *g_active = nullptr;
sfcop_guest_fn (*g_previousLookup)(std::uint32_t) = nullptr;

sfcop_guest_fn chainedLookup(std::uint32_t address) {
    if (g_active != nullptr) {
        if (sfcop_guest_fn found = g_active->Lookup(address)) {
            return found;
        }
    }
    return g_previousLookup != nullptr ? g_previousLookup(address) : nullptr;
}

// pcrecomp's shim row, as win32hle.h declares it.
struct ShimRow {
    const char *name;
    void (*fn)(void);
};

} // namespace

std::optional<GuestRuntime> clientRuntime() {
    using Reserve = std::uint32_t (*)(std::uint32_t, std::uint32_t);
    using Release = void (*)(std::uint32_t, std::uint32_t);
    using Resolve = std::uint32_t (*)(const char *);
    using Register = int (*)(const ShimRow *);
    using Lookup = sfcop_guest_fn (*)(std::uint32_t);
    const auto reserve = reinterpret_cast<Reserve>(native::programSymbol("hle_vm_reserve"));
    const auto release = reinterpret_cast<Release>(native::programSymbol("hle_vm_release"));
    const auto resolve = reinterpret_cast<Resolve>(native::programSymbol("hle_resolve"));
    const auto stub = reinterpret_cast<Resolve>(native::programSymbol("hle_resolve_or_stub"));
    const auto add = reinterpret_cast<Register>(native::programSymbol("win32hle_register"));
    const auto base = static_cast<const std::ptrdiff_t *>(native::programSymbol("g_mem_base"));
    const auto hook = static_cast<Lookup *>(native::programSymbol("hle_manual_hook"));
    if (reserve == nullptr || release == nullptr || resolve == nullptr || stub == nullptr ||
        add == nullptr || base == nullptr || hook == nullptr) {
        return std::nullopt;
    }
    GuestRuntime runtime;
    runtime.reserve = [reserve](std::uint32_t address, std::uint32_t size) {
        return reserve(address, size) == address;
    };
    runtime.release = release;
    runtime.memory = [base](std::uint32_t address) {
        return reinterpret_cast<std::uint8_t *>(static_cast<std::uintptr_t>(address) +
                                                static_cast<std::uintptr_t>(*base));
    };
    // The game's own bind found its rows in the client's modules. A mission imports three
    // KERNEL32 names the game does not (HeapReAlloc, HeapSize, TlsFree): pcrecomp's win32hle has
    // rows for the first two, registered here on first use; kernel32rt answers TlsFree.
    runtime.importAddress = [resolve, stub, add](const std::string &import) {
        static std::set<std::string> kept; // the registry and the stubs keep the names
        const std::string &name = *kept.insert(import.substr(import.find('!') + 1)).first;
        if (const std::uint32_t address = resolve(name.c_str())) {
            return address;
        }
        for (const char *table : {"win32hle_kernel32", "win32hle_kernel32_ext", "win32hle_kernel32_crt",
                                  "win32hle_user32", "win32hle_advapi32"}) {
            const auto *row = static_cast<const ShimRow *>(native::programSymbol(table));
            for (; row != nullptr && row->name != nullptr; ++row) {
                if (name == row->name) {
                    auto *entry = new ShimRow[2]{{name.c_str(), row->fn}, {nullptr, nullptr}};
                    add(entry); // kept for the run, as the registry keeps its tables
                    say("[scripts] %s: answered by pcrecomp's win32hle (no module owns it)\n", import);
                    return resolve(name.c_str());
                }
            }
        }
        say("[scripts] %s: no row answers it; the run ends if a mission calls it\n", import);
        return stub(name.c_str());
    };
    runtime.symbol = [](const char *name) { return native::programSymbol(name); };
    runtime.lookupHook = hook;
    return runtime;
}

std::filesystem::path missionModulesRoot() {
    if (const char *root = std::getenv("SFCOP_MISSION_MODULES"); root != nullptr && *root != '\0') {
        return std::filesystem::path(root);
    }
    std::error_code error;
    return std::filesystem::current_path(error) / "mods" / "missions";
}

GuestModuleProvider::GuestModuleProvider(ModuleLoader &loader, std::optional<GuestRuntime> runtime,
                                         std::filesystem::path root, const AllowList &allowList)
    : loader_(loader), runtime_(std::move(runtime)), root_(std::move(root)), allowList_(allowList) {}

GuestModuleProvider::~GuestModuleProvider() {
    if (g_active == this) {
        g_active = nullptr;
    }
}

bool GuestModuleProvider::Handles(std::string_view name) const {
    return isScript(name);
}

std::filesystem::path GuestModuleProvider::LibraryFor(std::string_view scriptName) const {
    const std::string name = isScript(scriptName) ? scriptStem(scriptName) : std::string(scriptName);
    const std::filesystem::path root = root_.empty() ? missionModulesRoot() : root_;
    return root / name / (name + std::string(kLibrarySuffix));
}

std::string GuestModuleProvider::CheckImage(const std::string &name, const sfcop_guest_image &image) const {
    const std::string refused = "refused: " + name + ": ";
    if (image.abi_version != SFCOP_GUEST_ABI_VERSION) {
        return refused + "its guest image is version " + std::to_string(image.abi_version) +
               ", the client reads version " + std::to_string(SFCOP_GUEST_ABI_VERSION);
    }
    const std::uint64_t end = std::uint64_t{image.base} + image.size;
    if (image.base < kLowestBase || end > kAboveModules || image.size == 0 ||
        image.base % kGranule != 0 || image.size % kGranule != 0) {
        return refused + "its guest image does not fit the modules' range";
    }
    for (const auto &loaded : byBase_) {
        const sfcop_guest_image &other = *loaded.second.image;
        if (image.base < other.base + other.size && other.base < end) {
            return refused + "its guest image overlaps " + loaded.second.name + "'s";
        }
    }
    if (image.bind == nullptr || image.call == nullptr || image.dispatch == nullptr ||
        image.dispatch_count == 0 || (image.segment_count != 0 && image.segments == nullptr) ||
        (image.import_count != 0 && image.imports == nullptr)) {
        return refused + "its guest image is incomplete";
    }
    for (std::uint32_t i = 0; i < image.segment_count; ++i) {
        const sfcop_guest_segment &segment = image.segments[i];
        if (std::uint64_t{segment.rva} + segment.size > image.size || segment.data_size > segment.size ||
            (segment.data_size != 0 && segment.data == nullptr)) {
            return refused + "a segment lies outside its guest image";
        }
    }
    for (std::uint32_t i = 0; i < image.dispatch_count; ++i) {
        const std::uint32_t address = image.dispatch[i].address;
        if (address < image.base || address >= end || image.dispatch[i].func == nullptr ||
            (i > 0 && address <= image.dispatch[i - 1].address)) {
            return refused + "its dispatch table is not inside its guest image, in order";
        }
    }
    const auto lifted = [&image](std::uint32_t address) {
        const sfcop_guest_dispatch *first = image.dispatch, *last = image.dispatch + image.dispatch_count;
        const auto *found = std::lower_bound(first, last, address,
                                             [](const sfcop_guest_dispatch &d, std::uint32_t a) { return d.address < a; });
        return found != last && found->address == address;
    };
    if (!lifted(image.mission_spec_entry) || !lifted(image.script_entry) ||
        (image.entry != 0 && !lifted(image.entry))) {
        return refused + "an entry point of its guest image was not lifted";
    }
    std::vector<std::string> imports;
    for (std::uint32_t i = 0; i < image.import_count; ++i) {
        const sfcop_guest_import &import = image.imports[i];
        if (import.name == nullptr || std::uint64_t{import.iat_rva} + 4 > image.size) {
            return refused + "an import slot lies outside its guest image";
        }
        imports.emplace_back(import.name);
    }
    if (const std::optional<LoadError> foreign = checkImports(name, imports, allowList_)) {
        return describe(*foreign);
    }
    return {};
}

std::uint32_t GuestModuleProvider::Load(std::string_view scriptName) {
    const std::string name = scriptStem(scriptName);
    for (auto &loaded : byBase_) {
        if (sameName(loaded.second.name, name)) { // as Windows compares a DLL's name
            ++loaded.second.references;
            return loaded.first;
        }
    }
    if (!isModuleName(name)) {
        say("[scripts] refused: %s: not a module name\n", name);
        return 0;
    }
    if (!runtime_) {
        say("[scripts] %s: not loaded: this program has no guest machine\n", name);
        return 0;
    }
    const LoadResult result = loader_.load(LibraryFor(name));
    if (!result.ok()) {
        say("[scripts] %s\n", describe(*result.error));
        return 0;
    }
    const sfcop_guest_image *image = result.module.guest;
    std::string why = image == nullptr ? "refused: " + name + ": it exports no " + std::string(kGuestImageName)
                                       : CheckImage(name, *image);
    if (why.empty() && !runtime_->reserve(image->base, image->size)) {
        why = "refused: " + name + ": its guest range is already in use";
    }
    if (!why.empty()) {
        say("[scripts] %s\n", why);
        loader_.unload(result.module.id);
        return 0;
    }
    std::uint8_t *memory = runtime_->memory(image->base);
    for (std::uint32_t i = 0; i < image->segment_count; ++i) {
        const sfcop_guest_segment &segment = image->segments[i];
        if (segment.data_size != 0) {
            std::memcpy(memory + segment.rva, segment.data, segment.data_size);
        }
    }
    for (std::uint32_t i = 0; i < image->import_count; ++i) {
        const std::uint32_t address = runtime_->importAddress(image->imports[i].name);
        std::memcpy(memory + image->imports[i].iat_rva, &address, sizeof address);
    }
    const Module module{name, result.module.id, image, 1};
    if (const char *missing = image->bind(runtime_->symbol)) {
        std::fprintf(stderr, "[scripts] refused: %s: the client's runtime has no %s\n", name.c_str(),
                     missing);
        Unmap(module);
        return 0;
    }
    byBase_[image->base] = module;
    InstallHook();
    if (image->entry != 0) {
        const std::uint32_t attach[3] = {image->base, 1u /* DLL_PROCESS_ATTACH */, 0u};
        if (image->call(image->entry, 3, attach) == 0) {
            say("[scripts] %s: its DLL entry point refused to attach\n", name);
            byBase_.erase(image->base);
            Unmap(module);
            return 0;
        }
    }
    std::fprintf(stderr, "[scripts] loaded %s at 0x%08X (%u functions, %u imports)\n", name.c_str(),
                 image->base, image->dispatch_count, image->import_count);
    return image->base;
}

bool GuestModuleProvider::Owns(std::uint32_t handle) const {
    return byBase_.count(handle) != 0;
}

std::uint32_t GuestModuleProvider::Export(std::uint32_t handle, std::string_view function) {
    const auto found = byBase_.find(handle);
    if (found == byBase_.end()) {
        return 0;
    }
    const sfcop_guest_image &image = *found->second.image;
    const std::uint32_t address = function == kMissionSpecEntryName ? image.mission_spec_entry
                                  : function == kScriptEntryName    ? image.script_entry
                                                                    : 0;
    std::fprintf(stderr, "[scripts] %s!%.*s: 0x%08X\n", found->second.name.c_str(),
                 static_cast<int>(function.size()), function.data(), address);
    return address;
}

void GuestModuleProvider::Free(std::uint32_t handle) {
    const auto found = byBase_.find(handle);
    if (found == byBase_.end() || --found->second.references != 0) {
        return;
    }
    const Module module = found->second;
    if (module.image->entry != 0) {
        const std::uint32_t detach[3] = {module.image->base, 0u /* DLL_PROCESS_DETACH */, 0u};
        module.image->call(module.image->entry, 3, detach);
    }
    byBase_.erase(found);
    Unmap(module);
    say("[scripts] unloaded %s\n", module.name);
}

sfcop_guest_fn GuestModuleProvider::Lookup(std::uint32_t address) const {
    auto found = byBase_.upper_bound(address);
    if (found == byBase_.begin()) {
        return nullptr;
    }
    const sfcop_guest_image &image = *(--found)->second.image;
    if (address - image.base >= image.size) {
        return nullptr;
    }
    const sfcop_guest_dispatch *first = image.dispatch, *last = image.dispatch + image.dispatch_count;
    const auto *at = std::lower_bound(first, last, address,
                                      [](const sfcop_guest_dispatch &d, std::uint32_t a) { return d.address < a; });
    return at != last && at->address == address ? at->func : nullptr;
}

void GuestModuleProvider::Unmap(const Module &module) {
    runtime_->release(module.image->base, module.image->size);
    loader_.unload(module.id);
}

void GuestModuleProvider::InstallHook() {
    if (g_active == this || runtime_->lookupHook == nullptr) {
        g_active = this;
        return;
    }
    if (*runtime_->lookupHook != chainedLookup) {
        g_previousLookup = *runtime_->lookupHook;
        *runtime_->lookupHook = chainedLookup;
    }
    g_active = this;
}

} // namespace sfcop::scripts

namespace sfcop::hle::kernel32rt {

ModuleProvider &MissionModules() {
    static const std::unique_ptr<scripts::ModuleLoader> loader =
        scripts::makeNativeModuleLoader(scripts::builtInAllowList(), scripts::builtInHostBaseline());
    static scripts::GuestModuleProvider provider(*loader, scripts::clientRuntime(), {},
                                                 scripts::builtInAllowList());
    return provider;
}

} // namespace sfcop::hle::kernel32rt
