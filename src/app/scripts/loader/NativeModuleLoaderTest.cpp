// The real loader on modules the test compiles itself with the host's C compiler: a conforming
// module loads and both entry points answer; each refusal of ModuleContract.md §5 is made, in
// order, with its message; checkLookup() follows §3's dynamic-lookup rule; and the import-list
// fixtures get the verdicts tools/scr2native/gen_allowlist.py --check gives them.
//
// The modules are built under the working directory (the build tree when CTest runs the tests).
// A host without a C compiler, or one whose platform loader is not written yet, skips these.
#include "app/scripts/loader/NativeModuleLoader.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

using namespace sfcop::scripts;

namespace {

#if defined(__APPLE__) || defined(__linux__)
constexpr bool kCanBuild = true;
#else
constexpr bool kCanBuild = false;
#endif
#if defined(__APPLE__)
constexpr const char *kSuffix = ".dylib";
#else
constexpr const char *kSuffix = ".so";
#endif

const std::filesystem::path kHere = std::filesystem::path(__FILE__).parent_path().parent_path();

std::string readText(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot read " << path.string());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeText(const std::filesystem::path &path, const std::string &text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

std::vector<std::string> readImports(const char *fixture) {
    std::vector<std::string> out;
    std::istringstream in(readText(kHere / "fixtures" / fixture));
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line[0] != '#') {
            out.push_back(line);
        }
    }
    return out;
}

const AllowList &committedAllowList() {
    static const AllowList list = AllowList::parse(readText(kHere / "allowlist.txt"));
    return list;
}

bool haveCompiler() {
    static const bool have = kCanBuild && std::system("cc --version >/dev/null 2>&1") == 0;
    return have;
}

// What a test module is made of.
struct Spec {
    std::string name = "Probe";
    std::vector<std::string> imports; // allow-list names, imported as kImportPrefix + name
    bool tag = true;
    unsigned contractVersion = kModuleContractVersion;
    bool missionSpecEntry = true;
    bool scriptEntry = true;
    bool callsSystem = false; // a host import that is on no baseline list
    std::string manifest;     // module.json; empty: a valid one
};

std::filesystem::path root() {
    return std::filesystem::temp_directory_path() / "sfcop-native-module-tests";
}

// Compiles the module to <root>/<name>/<name><suffix> beside its module.json; the library's path.
std::filesystem::path build(const Spec &spec) {
    const std::filesystem::path dir = root() / spec.name;
    std::filesystem::remove_all(dir);
    std::ostringstream c;
    c << "#include <stdlib.h>\n#define EXPORT __attribute__((visibility(\"default\")))\n";
    if (spec.tag) {
        c << "struct tag { char magic[8]; unsigned contract; float script; char abi[32]; };\n"
          << "EXPORT const struct tag sfcop_module_abi = {{'S','F','C','O','P','M','O','D'}, "
          << spec.contractVersion << "u, 22.0f, \"" << kHostAbiTag << "\"};\n";
    }
    if (spec.missionSpecEntry) {
        c << "EXPORT const void *MissionSpecEntry(float v) { return v == 22.0f ? (const void *)0x1234 : 0; }\n";
    }
    if (spec.scriptEntry) {
        c << "EXPORT void *ScriptEntry(void) { return (void *)0x5678; }\n";
    }
    std::string link;
    for (std::size_t i = 0; i < spec.imports.size(); ++i) {
        const std::string symbol = std::string(kImportPrefix) + spec.imports[i];
#if defined(__APPLE__)
        c << "extern char import" << i << " __asm__(\"_" << symbol << "\") __attribute__((weak_import));\n";
        link += " '-Wl,-U,_" + symbol + "'";
#else
        c << "extern char import" << i << " __asm__(\"" << symbol << "\") __attribute__((weak));\n";
#endif
        c << "EXPORT void *ref" << i << "(void) { return &import" << i << "; }\n";
    }
    if (spec.callsSystem) {
        c << "EXPORT int run(const char *s) { return system(s); }\n";
    }
    writeText(dir / "module.c", c.str());
    writeText(dir / "module.json",
              spec.manifest.empty() ? "{\"name\": \"" + spec.name + "\", \"source_sha256\": \"" +
                                          std::string(64, 'a') + "\", \"translator_version\": \"1.0.0\"}\n"
                                    : spec.manifest);
    const std::filesystem::path library = dir / (spec.name + kSuffix);
#if defined(__APPLE__)
    const std::string shared = "-dynamiclib";
#else
    const std::string shared = "-shared -fPIC";
#endif
    const std::string command = "cc -w -O1 -fvisibility=hidden " + shared + " '" + (dir / "module.c").string() +
                                "' -o '" + library.string() + "'" + link + " >'" +
                                (dir / "cc.log").string() + "' 2>&1";
    REQUIRE_MESSAGE(std::system(command.c_str()) == 0, "cc failed: " << readText(dir / "cc.log"));
    return library;
}

std::unique_ptr<ModuleLoader> loader() {
    return makeNativeModuleLoader(committedAllowList(), AllowList::parse("memcpy\n"));
}

std::string refusalFor(const Spec &spec) {
    const std::filesystem::path library = build(spec);
    auto modules = loader();
    const std::optional<LoadError> verified = modules->verify(library);
    REQUIRE(verified.has_value());
    const LoadResult loaded = modules->load(library);
    REQUIRE_FALSE(loaded.ok());
    CHECK(describe(*loaded.error) == describe(*verified)); // load() refuses as verify() does
    return describe(*verified);
}

} // namespace

TEST_SUITE("native module loader") {

TEST_CASE("a conforming module loads, and both entry points answer from the library") {
    if (!haveCompiler()) {
        MESSAGE("skipped: no C compiler for this host");
        return;
    }
    Spec spec;
    spec.imports = {"KERNEL32.DLL!HeapAlloc", "??0tShipInfo@@QAE@XZ"};
    const std::filesystem::path library = build(spec);
    auto modules = loader();
    CHECK_FALSE(modules->verify(library).has_value());
    const LoadResult loaded = modules->load(library);
    REQUIRE(loaded.ok());
    CHECK(loaded.module.manifest.name == "Probe");
    REQUIRE(loaded.module.missionSpecEntry != nullptr);
    REQUIRE(loaded.module.scriptEntry != nullptr);
    CHECK(reinterpret_cast<std::uintptr_t>(loaded.module.missionSpecEntry(22.0f)) == 0x1234u);
    CHECK(reinterpret_cast<std::uintptr_t>(loaded.module.scriptEntry()) == 0x5678u);
    CHECK(loaded.module.guest == nullptr); // a hand-made module carries no guest image
    const LoadResult again = modules->load(library);
    REQUIRE(again.ok());
    CHECK(again.module.id == loaded.module.id);
    modules->unload(loaded.module.id);
    CHECK(modules->checkLookup(loaded.module.id, "", "ScriptEntry")->reason == Refusal::kNotFound);
}

TEST_CASE("each refusal of the contract is made on a real file, with its message, in order") {
    if (!haveCompiler()) {
        MESSAGE("skipped: no C compiler for this host");
        return;
    }
    auto modules = loader();
    CHECK(describe(*modules->verify(root() / "Absent" / (std::string("Absent") + kSuffix))) ==
          "refused: Absent: not found");

    Spec noManifest;
    noManifest.name = "NoManifest";
    const std::filesystem::path library = build(noManifest);
    std::filesystem::remove(library.parent_path() / "module.json");
    CHECK(describe(*modules->verify(library)) == "refused: NoManifest: no module.json beside the library");

    Spec badManifest;
    badManifest.name = "BadManifest";
    badManifest.manifest = "{\"name\": \"BadManifest\", \"source_sha256\": \"xyz\", \"translator_version\": \"1\"}";
    CHECK(refusalFor(badManifest) == "refused: BadManifest: module.json is invalid: source_sha256");

    Spec notLoadable;
    notLoadable.name = "NotLoadable";
    const std::filesystem::path text = build(notLoadable);
    writeText(text, "not a library\n");
    const std::string reason = describe(*modules->verify(text));
    CHECK(reason.rfind("refused: NotLoadable: not a loadable module for this host: ", 0) == 0);

    Spec noTag;
    noTag.name = "NoTag";
    noTag.tag = false;
    CHECK(refusalFor(noTag) == "refused: NoTag: no sfcop_module_abi version tag");

    Spec mismatch;
    mismatch.name = "Mismatch";
    mismatch.contractVersion = 2;
    mismatch.imports = {"KERNEL32.DLL!CreateProcessA"}; // the tag is checked first
    CHECK(refusalFor(mismatch) == "refused: Mismatch: version tag mismatch: contract version 2, expected 1");

    Spec foreign;
    foreign.name = "Foreign";
    foreign.imports = {"KERNEL32.DLL!CreateProcessA"};
    foreign.scriptEntry = false; // the imports are checked before the entry points
    CHECK(refusalFor(foreign) == "refused: Foreign: imports KERNEL32.DLL!CreateProcessA, which is outside "
                                 "the scripting library and the C runtime");

    Spec host;
    host.name = "HostRuntime";
    host.callsSystem = true;
    CHECK(refusalFor(host) == "refused: HostRuntime: imports system, which is outside the scripting library "
                              "and the C runtime");

    Spec noEntry;
    noEntry.name = "NoEntry";
    noEntry.scriptEntry = false;
    CHECK(refusalFor(noEntry) == "refused: NoEntry: does not export ScriptEntry");
}

TEST_CASE("modules opened ahead load and refuse exactly as modules opened at load") {
    if (!haveCompiler()) {
        MESSAGE("skipped: no C compiler for this host");
        return;
    }
    Spec first;
    first.name = "AheadFirst";
    Spec second;
    second.name = "AheadSecond";
    second.imports = {"KERNEL32.DLL!HeapAlloc"};
    Spec foreign;
    foreign.name = "AheadForeign";
    foreign.imports = {"KERNEL32.DLL!CreateProcessA"};
    Spec notAhead;
    notAhead.name = "NotAhead";
    const std::filesystem::path a = build(first), b = build(second), f = build(foreign), n = build(notAhead);

    auto modules = makeNativeModuleLoader(committedAllowList(), AllowList::parse("memcpy\n"), {a, b, f});
    const LoadResult loadedB = modules->load(b); // out of order: taken, or worked here
    REQUIRE(loadedB.ok());
    CHECK(reinterpret_cast<std::uintptr_t>(loadedB.module.scriptEntry()) == 0x5678u);
    CHECK(loadedB.module.manifest.name == "AheadSecond");
    const LoadResult loadedA = modules->load(a);
    REQUIRE(loadedA.ok());
    CHECK(reinterpret_cast<std::uintptr_t>(loadedA.module.missionSpecEntry(22.0f)) == 0x1234u);
    CHECK(loadedA.module.id != loadedB.module.id);
    const LoadResult refused = modules->load(f);
    REQUIRE_FALSE(refused.ok());
    CHECK(describe(*refused.error) == "refused: AheadForeign: imports KERNEL32.DLL!CreateProcessA, which is "
                                      "outside the scripting library and the C runtime");
    const LoadResult other = modules->load(n); // not in the list: opened at load, as before
    REQUIRE(other.ok());
    CHECK(other.module.manifest.name == "NotAhead");

    // Unloaded and loaded again: opened again at load, a new module.
    modules->unload(loadedA.module.id);
    CHECK(modules->checkLookup(loadedA.module.id, "", "ScriptEntry")->reason == Refusal::kNotFound);
    const LoadResult again = modules->load(a);
    REQUIRE(again.ok());
    CHECK(again.module.id != loadedA.module.id);
    CHECK(reinterpret_cast<std::uintptr_t>(again.module.scriptEntry()) == 0x5678u);
}

TEST_CASE("a loader destroyed with modules opened ahead and never loaded closes them") {
    if (!haveCompiler()) {
        MESSAGE("skipped: no C compiler for this host");
        return;
    }
    Spec spec;
    spec.name = "AheadUnused";
    const std::filesystem::path library = build(spec);
    for (int i = 0; i < 3; ++i) { // destroyed while working, and after
        auto modules = makeNativeModuleLoader(committedAllowList(), AllowList::parse("memcpy\n"), {library});
    }
    auto modules = makeNativeModuleLoader(committedAllowList(), AllowList::parse("memcpy\n"), {library});
    CHECK(modules->load(library).ok());
}

TEST_CASE("checkLookup on a loaded module follows the contract's dynamic-lookup rule") {
    if (!haveCompiler()) {
        MESSAGE("skipped: no C compiler for this host");
        return;
    }
    Spec spec;
    spec.name = "Lookup";
    const std::filesystem::path library = build(spec);
    auto modules = loader();
    const LoadResult loaded = modules->load(library);
    REQUIRE(loaded.ok());
    const ModuleId id = loaded.module.id;
    CHECK_FALSE(modules->checkLookup(id, "", "ScriptEntry").has_value());
    CHECK_FALSE(modules->checkLookup(id, "", "").has_value());
    CHECK(describe(*modules->checkLookup(id, "", "Other")) ==
          "refused: Lookup: imports Other, which is outside the scripting library and the C runtime");
    CHECK_FALSE(modules->checkLookup(id, "kernel32", "HeapAlloc").has_value());
    CHECK_FALSE(modules->checkLookup(id, "KERNEL32.DLL", "").has_value());
    CHECK(describe(*modules->checkLookup(id, "kernel32.dll", "CreateProcessA")) ==
          "refused: Lookup: imports KERNEL32.DLL!CreateProcessA, which is outside the scripting library "
          "and the C runtime");
    CHECK(describe(*modules->checkLookup(id, "wsock32", "")) ==
          "refused: Lookup: imports WSOCK32.DLL, which is outside the scripting library and the C runtime");
    CHECK(modules->checkLookup(id + 1, "", "ScriptEntry")->reason == Refusal::kNotFound);
}

TEST_CASE("the import-list fixtures get gen_allowlist.py --check's verdicts from the real loader") {
    if (!haveCompiler()) {
        MESSAGE("skipped: no C compiler for this host");
        return;
    }
    Spec conforming;
    conforming.name = "Conforming";
    conforming.imports = readImports("conforming.imports");
    auto modules = loader();
    CHECK_FALSE(modules->verify(build(conforming)).has_value());

    Spec foreign;
    foreign.name = "ForeignFixture";
    foreign.imports = readImports("foreign.imports");
    CHECK(refusalFor(foreign) == "refused: ForeignFixture: imports KERNEL32.DLL!CreateProcessA, which is "
                                 "outside the scripting library and the C runtime");
}

} // TEST_SUITE

TEST_CASE("a file of another format or architecture is not loadable, with the reason") {
    CHECK(readLibrary("").notLoadable != "");
    CHECK(readLibrary("not a library").notLoadable != "");
    const std::string elf = std::string("\x7f" "ELF\x01\x01\x01", 7) + std::string(57, '\0');
    CHECK(readLibrary(elf, "arm64-linux-itanium").notLoadable != "");
    const std::string pe = "MZ" + std::string(62, '\0');
    CHECK(readLibrary(pe, "x86_64-windows-msvc").notLoadable != "");
}

TEST_CASE("module.json is read field by field, naming the first invalid one") {
    const std::string digest(64, 'b');
    const ManifestRead good =
        parseManifest("{\"name\": \"Ski_1\", \"source_sha256\": \"" + digest + "\", \"translator_version\": \"1.0.0\"}");
    REQUIRE(good.manifest.has_value());
    CHECK(good.manifest->name == "Ski_1");
    CHECK(good.manifest->sourceSha256 == digest);
    CHECK(good.manifest->translatorVersion == "1.0.0");
    CHECK(parseManifest("{\"source_sha256\": \"" + digest + "\", \"translator_version\": \"1\"}").error == "name");
    CHECK(parseManifest("{\"name\": \"A\", \"source_sha256\": \"" + std::string(64, 'B') +
                        "\", \"translator_version\": \"1\"}")
              .error == "source_sha256");
    CHECK_FALSE(parseManifest("not json").manifest.has_value());
}
