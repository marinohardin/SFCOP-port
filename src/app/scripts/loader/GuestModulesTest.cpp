// The provider behind LoadLibraryA for ".scr" names, on a fake guest machine: a guest image is
// checked before anything is written, then mapped, bound, answered in the dispatch and attached;
// refusals leave nothing behind; FreeLibrary detaches and releases at the last reference.
#include "app/scripts/loader/GuestModules.h"

#include <doctest/doctest.h>

#include "app/FilesMock.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <random>
#include <set>
#include <system_error>

using namespace sfcop::scripts;

namespace {

constexpr std::uint32_t kBase = 0x60300000u;
constexpr std::uint32_t kSize = 0x00020000u;

int g_lifted = 0;
void liftedEntry() { g_lifted = 1; }
void liftedMissionSpec() { g_lifted = 2; }
void liftedScript() { g_lifted = 3; }
void gameFunction() { g_lifted = 4; }

const std::uint8_t kText[] = {0x55, 0x8B, 0xEC};
const std::uint8_t kData[] = {1, 2, 3, 4};
const sfcop_guest_segment kSegments[] = {{0x1000, 0x1000, sizeof kText, kText},
                                         {0x3000, 0x2000, sizeof kData, kData}};
const sfcop_guest_segment kPastTheImage[] = {{0x1F000, 0x2000, sizeof kText, kText}};
const sfcop_guest_dispatch kOutOfOrder[] = {{kBase + 0x1020, liftedScript}, {kBase + 0x1010, liftedMissionSpec}};
const sfcop_guest_import kImports[] = {{0x2000, "KERNEL32.DLL!HeapAlloc"}, {0x2004, "USER32.DLL!wsprintfA"}};
const sfcop_guest_import kForeign[] = {{0x2000, "KERNEL32.DLL!CreateProcessA"}};
const sfcop_guest_dispatch kDispatch[] = {
    {kBase + 0x1000, liftedEntry}, {kBase + 0x1010, liftedMissionSpec}, {kBase + 0x1020, liftedScript}};

struct Calls {
    std::vector<std::vector<std::uint32_t>> calls;
    std::uint32_t attachAnswer = 1;
    const char *bindMissing = nullptr;
    int binds = 0;
} g_calls;

const char *fakeBind(sfcop_guest_resolver resolve) {
    ++g_calls.binds;
    CHECK(resolve != nullptr);
    return g_calls.bindMissing;
}

std::uint32_t fakeCall(std::uint32_t address, std::uint32_t argc, const std::uint32_t *args) {
    std::vector<std::uint32_t> call{address};
    call.insert(call.end(), args, args + argc);
    g_calls.calls.push_back(call);
    return g_calls.attachAnswer;
}

sfcop_guest_image image() {
    sfcop_guest_image i{};
    i.abi_version = SFCOP_GUEST_ABI_VERSION;
    i.base = kBase;
    i.size = kSize;
    i.entry = kBase + 0x1000;
    i.mission_spec_entry = kBase + 0x1010;
    i.script_entry = kBase + 0x1020;
    i.segments = kSegments;
    i.segment_count = 2;
    i.imports = kImports;
    i.import_count = 2;
    i.dispatch = kDispatch;
    i.dispatch_count = 3;
    i.bind = fakeBind;
    i.call = fakeCall;
    return i;
}

// Hands out one module whose guest image the test sets.
class FakeLoader final : public ModuleLoader {
public:
    sfcop_guest_image guest = image();
    bool refuse = false;
    bool noGuest = false;
    std::vector<std::filesystem::path> asked;
    std::set<ModuleId> loaded;
    ModuleId next = 7;

    std::optional<LoadError> verify(const std::filesystem::path &) override { return std::nullopt; }
    LoadResult load(const std::filesystem::path &library) override {
        asked.push_back(library);
        if (refuse) {
            return LoadResult{{}, LoadError{Refusal::kForeignImport, library.stem().string(), "system"}};
        }
        LoadedModule module;
        module.id = next++;
        module.guest = noGuest ? nullptr : &guest;
        loaded.insert(module.id);
        return LoadResult{module, std::nullopt};
    }
    void unload(ModuleId id) override { loaded.erase(id); }
    std::optional<LoadError> checkLookup(ModuleId, std::string_view, std::string_view) override {
        return std::nullopt;
    }
};

// The guest's memory for one module slot.
struct FakeGuest {
    std::vector<std::uint8_t> memory = std::vector<std::uint8_t>(kSize, 0xCC);
    std::set<std::uint32_t> reserved;
    bool full = false;
    std::vector<std::string> imports;

    GuestRuntime runtime(sfcop_guest_fn (**hook)(std::uint32_t) = nullptr) {
        GuestRuntime r;
        r.reserve = [this](std::uint32_t address, std::uint32_t size) {
            if (full || address != kBase || size != kSize || reserved.count(address) != 0) {
                return false;
            }
            std::fill(memory.begin(), memory.end(), 0);
            reserved.insert(address);
            return true;
        };
        r.release = [this](std::uint32_t address, std::uint32_t) { reserved.erase(address); };
        r.memory = [this](std::uint32_t address) { return memory.data() + (address - kBase); };
        r.importAddress = [this](const std::string &name) {
            imports.push_back(name);
            return 0x7F000000u + 16u * static_cast<std::uint32_t>(imports.size());
        };
        r.symbol = [](const char *) -> void * { return nullptr; };
        r.lookupHook = hook;
        return r;
    }
    std::uint32_t word(std::uint32_t address) const {
        std::uint32_t v;
        std::memcpy(&v, memory.data() + (address - kBase), 4);
        return v;
    }
};

const AllowList &allowList() {
    static const AllowList list =
        AllowList::parse("KERNEL32.DLL!HeapAlloc\nUSER32.DLL!wsprintfA\n");
    return list;
}

} // namespace

TEST_CASE("the provider answers .scr names only, in any case, with or without a directory") {
    FakeLoader loader;
    GuestModuleProvider provider(loader, std::nullopt, "mods", allowList());
    CHECK(provider.Handles("Ski_1Free4All.scr"));
    CHECK(provider.Handles("Assets\\Scripts\\Bon_Hit.SCR"));
    CHECK(provider.Handles("assets/scripts/x.Scr"));
    CHECK_FALSE(provider.Handles("OSSpecific95.dll"));
    CHECK_FALSE(provider.Handles(".scr"));
    CHECK_FALSE(provider.Handles("scr"));
#if defined(__APPLE__)
    CHECK(provider.LibraryFor("Assets\\Scripts\\Bon_Hit.scr") ==
          std::filesystem::path("mods") / "Bon_Hit" / "Bon_Hit.dylib");
#endif
}

TEST_CASE("a translated module is mapped, bound, attached and answered at its guest addresses") {
    g_calls = Calls{};
    FakeLoader loader;
    FakeGuest guest;
    sfcop_guest_fn (*hook)(std::uint32_t) = [](std::uint32_t a) -> sfcop_guest_fn {
        return a == 0x00401000u ? gameFunction : nullptr;
    };
    GuestModuleProvider provider(loader, guest.runtime(&hook), "mods", allowList());

    const std::uint32_t handle = provider.Load("Assets\\Scripts\\Probe.scr");
    REQUIRE(handle == kBase);
    CHECK(loader.asked.back().filename().string().rfind("Probe.", 0) == 0);
    CHECK(provider.Owns(kBase));
    CHECK(guest.memory[0x1000] == 0x55);
    CHECK(guest.memory[0x1003] == 0); // the rest of a segment is zero
    CHECK(guest.memory[0x3003] == 4);
    CHECK(guest.word(kBase + 0x2000) == 0x7F000010u); // each import slot holds its row
    CHECK(guest.word(kBase + 0x2004) == 0x7F000020u);
    CHECK(guest.imports == std::vector<std::string>{"KERNEL32.DLL!HeapAlloc", "USER32.DLL!wsprintfA"});
    CHECK(g_calls.binds == 1);
    REQUIRE(g_calls.calls.size() == 1);
    CHECK(g_calls.calls[0] == std::vector<std::uint32_t>{kBase + 0x1000, kBase, 1, 0}); // attach

    CHECK(provider.Export(handle, "MissionSpecEntry") == kBase + 0x1010);
    CHECK(provider.Export(handle, "ScriptEntry") == kBase + 0x1020);
    CHECK(provider.Export(handle, "DllMain") == 0);
    CHECK(provider.Export(handle + 1, "ScriptEntry") == 0);

    CHECK(provider.Lookup(kBase + 0x1020) == liftedScript);
    CHECK(provider.Lookup(kBase + 0x1024) == nullptr);
    CHECK(provider.Lookup(kBase + kSize) == nullptr);
    CHECK(provider.Lookup(0x00401000u) == nullptr);
    REQUIRE(hook != nullptr); // the dispatch asks the provider, then the hook it replaced
    CHECK(hook(kBase + 0x1010) == liftedMissionSpec);
    CHECK(hook(0x00401000u) == gameFunction);
    CHECK(hook(0x00402000u) == nullptr);

    CHECK(provider.Load("probe.SCR") == handle); // names are compared without regard to case
    CHECK(loader.asked.size() == 1);
    provider.Free(handle);
    provider.Free(handle);
    CHECK_FALSE(provider.Owns(handle));
}

TEST_CASE("a second LoadLibraryA counts a reference; the last FreeLibrary detaches and releases") {
    g_calls = Calls{};
    FakeLoader loader;
    FakeGuest guest;
    GuestModuleProvider provider(loader, guest.runtime(), "mods", allowList());
    const std::uint32_t handle = provider.Load("Probe.scr");
    REQUIRE(handle == kBase);
    CHECK(provider.Load("Assets\\Scripts\\Probe.scr") == handle);
    CHECK(loader.asked.size() == 1);
    provider.Free(handle);
    CHECK(provider.Owns(handle));
    CHECK(g_calls.calls.size() == 1);
    provider.Free(handle);
    CHECK_FALSE(provider.Owns(handle));
    REQUIRE(g_calls.calls.size() == 2);
    CHECK(g_calls.calls[1] == std::vector<std::uint32_t>{kBase + 0x1000, kBase, 0, 0}); // detach
    CHECK(guest.reserved.empty());
    CHECK(loader.loaded.empty());
    CHECK(provider.Lookup(kBase + 0x1020) == nullptr);
    provider.Free(handle); // a handle no longer loaded is ignored
}

TEST_CASE("a refused module leaves no reservation, no loaded library and no handle") {
    struct Case {
        const char *what;
        void (*change)(FakeLoader &, FakeGuest &);
    };
    const Case cases[] = {
        {"the loader refuses it", [](FakeLoader &l, FakeGuest &) { l.refuse = true; }},
        {"another guest image version", [](FakeLoader &l, FakeGuest &) { l.guest.abi_version = 2; }},
        {"a base outside the modules' range", [](FakeLoader &l, FakeGuest &) { l.guest.base = 0x00400000u; }},
        {"a size off the 64 KB granule", [](FakeLoader &l, FakeGuest &) { l.guest.size = 0x1234; }},
        {"a segment past the image", [](FakeLoader &l, FakeGuest &) {
             l.guest.segments = kPastTheImage;
             l.guest.segment_count = 1;
         }},
        {"a dispatch table out of order", [](FakeLoader &l, FakeGuest &) {
             l.guest.dispatch = kOutOfOrder;
             l.guest.dispatch_count = 2;
         }},
        {"an export that was not lifted", [](FakeLoader &l, FakeGuest &) { l.guest.script_entry = kBase + 4; }},
        {"a foreign import", [](FakeLoader &l, FakeGuest &) {
             l.guest.imports = kForeign;
             l.guest.import_count = 1;
         }},
        {"guest memory already in use", [](FakeLoader &, FakeGuest &g) { g.full = true; }},
        {"a runtime symbol the client lacks", [](FakeLoader &, FakeGuest &) { g_calls.bindMissing = "g_eax"; }},
        {"a DLL entry point that refuses", [](FakeLoader &, FakeGuest &) { g_calls.attachAnswer = 0; }},
        {"no guest image", [](FakeLoader &l, FakeGuest &) { l.noGuest = true; }},
    };
    for (const Case &c : cases) {
        CAPTURE(c.what);
        g_calls = Calls{};
        FakeLoader loader;
        FakeGuest guest;
        c.change(loader, guest);
        GuestModuleProvider provider(loader, guest.runtime(), "mods", allowList());
        CHECK(provider.Load("Probe.scr") == 0);
        CHECK_FALSE(provider.Owns(kBase));
        CHECK(guest.reserved.empty());
        CHECK(loader.loaded.empty());
        CHECK(provider.Lookup(kBase + 0x1020) == nullptr);
    }
}

TEST_CASE("without a guest machine, or for a name that is not a module name, nothing is loaded") {
    FakeLoader loader;
    GuestModuleProvider none(loader, std::nullopt, "mods", allowList());
    CHECK(none.Load("Probe.scr") == 0);
    CHECK(loader.asked.empty());
    FakeGuest guest;
    GuestModuleProvider provider(loader, guest.runtime(), "mods", allowList());
    CHECK(provider.Load("bad name.scr") == 0);
    CHECK(provider.Load(".hidden.scr") == 0);
    CHECK(loader.asked.empty());
}

TEST_CASE("the modules root is $SFCOP_MISSION_MODULES, else mods/missions in the working directory") {
    // Read only: the tests do not change the process environment.
    const char *set = std::getenv("SFCOP_MISSION_MODULES");
    if (set == nullptr || *set == '\0') {
        CHECK(missionModulesRoot() == std::filesystem::current_path() / "mods" / "missions");
    } else {
        CHECK(missionModulesRoot() == std::filesystem::path(set));
    }
    CHECK_FALSE(clientRuntime().has_value()); // the test runner is not the lifted client
}

TEST_CASE("the environment wins, then the folder named in the data folder, then mods/missions") {
    sfcop::app::testing::MemoryFileSystem files;
    const std::string data = "/Users/testcaptain/Library/Application Support/SFCOP";
    CHECK(missionModulesRootFor(std::nullopt, data, files) ==
          std::filesystem::path(data + "/mods/missions"));
    CHECK(missionModulesRootFor(std::string(), data, files) ==
          std::filesystem::path(data + "/mods/missions")); // set but empty is unset
    files.files[data + "/mission-modules.txt"] = "/work/missions/modules\n";
    CHECK(missionModulesRootFor(std::nullopt, data, files) ==
          std::filesystem::path("/work/missions/modules"));
    CHECK(missionModulesRootFor(std::string("/elsewhere/modules"), data, files) ==
          std::filesystem::path("/elsewhere/modules"));
}

TEST_CASE("the data folder the host names wins over the working directory the game moves") {
    const std::filesystem::path top =
        std::filesystem::temp_directory_path() /
        ("sfcop-modulesroot-" + std::to_string(std::random_device{}()));
    std::error_code ignored;
    std::filesystem::remove_all(top, ignored);
    const std::filesystem::path data = top / "data", install = top / "install";
    std::filesystem::create_directories(data);
    std::filesystem::create_directories(install); // no mission-modules.txt here
    std::ofstream(data / "mission-modules.txt") << "/work/missions/modules\n";
    const std::filesystem::path before = std::filesystem::current_path();

    setMissionModulesDataDir(data);
    std::filesystem::current_path(install); // as the game's SetCurrentDirectoryA does during boot
    const std::filesystem::path root = missionModulesRoot();
    setMissionModulesDataDir({});
    const std::filesystem::path unset = missionModulesRoot();
    const std::filesystem::path moved = std::filesystem::current_path(); // the install, as the OS spells it
    std::filesystem::current_path(before);
    std::filesystem::remove_all(top, ignored);

    // Read only, as above: the tests do not change the process environment.
    const char *set = std::getenv("SFCOP_MISSION_MODULES");
    if (set == nullptr || *set == '\0') {
        CHECK(root == std::filesystem::path("/work/missions/modules"));
        // Unset, the working directory serves as the data folder, as for the tests and the tools.
        CHECK(unset == moved / "mods" / "missions");
    } else {
        CHECK(root == std::filesystem::path(set));
    }
}

TEST_CASE("two missions lifted at one base: the second is refused while the first is loaded") {
    g_calls = Calls{};
    FakeLoader loader; // every library it hands out holds the same guest image
    FakeGuest guest;
    GuestModuleProvider provider(loader, guest.runtime(), "mods", allowList());
    const std::uint32_t first = provider.Load("First.scr");
    REQUIRE(first == kBase);
    CHECK(provider.Load("Second.scr") == 0);
    CHECK(provider.Owns(first));
    CHECK(loader.loaded.size() == 1); // the second's library was unloaded again
    provider.Free(first);
    CHECK(provider.Load("Second.scr") == kBase); // free, the slot takes the other
    CHECK(provider.Owns(kBase));
}

TEST_CASE("the libraries opened ahead are the modules LibraryFor names, in byte order of their names") {
#if defined(__APPLE__)
    const std::string suffix = ".dylib";
#elif defined(_WIN32)
    const std::string suffix = ".dll";
#else
    const std::string suffix = ".so";
#endif
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("sfcop-missionlibraries-" + std::to_string(std::random_device{}()) + "-" +
         std::to_string(reinterpret_cast<std::uintptr_t>(&suffix)));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    const auto add = [&root, &suffix](const std::string &folder, const std::string &file) {
        std::filesystem::create_directories(root / folder);
        if (!file.empty()) {
            std::ofstream(root / folder / (file + suffix)) << "x";
        }
    };
    add("Met_1Scout", "Met_1Scout");
    add("Bon_Hit", "Bon_Hit");
    add("Met_19Scan", "Met_19Scan");
    add("Empty", "");              // no library: not a module
    add("Wrong", "Other");         // a library named for another folder: LibraryFor would not name it
    add("bad name", "bad name");   // not a module name
    std::ofstream(root / ("Loose" + suffix)) << "x"; // a file, not a folder
    CHECK(missionLibraries(root) ==
          std::vector<std::filesystem::path>{root / "Bon_Hit" / ("Bon_Hit" + suffix),
                                             root / "Met_19Scan" / ("Met_19Scan" + suffix),
                                             root / "Met_1Scout" / ("Met_1Scout" + suffix)});
    CHECK(missionLibraries(root / "absent").empty());
    std::filesystem::remove_all(root, ignored);
}
