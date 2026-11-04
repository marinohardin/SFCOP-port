// The module contract through the mock loader: the committed allow-list and the import-list
// fixtures beside this file (shared with tests/test_module_imports.py).
#include "app/scripts/ModuleLoader.h"
#include "app/scripts/ModuleLoaderMock.h"

#include <doctest/doctest.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

using namespace sfcop::scripts;

namespace {

const std::filesystem::path kHere = std::filesystem::path(__FILE__).parent_path();

std::string readText(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot read " << path.string());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
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

// A module as scr2native would make it, importing `imports` from the scripting library.
ModuleImage imageOf(const std::string &name, const std::vector<std::string> &imports) {
    ModuleImage image;
    image.manifest = ModuleManifest{name, std::string(64, 'a'), "0.1.0"};
    image.abiTag = hostAbiTag();
    for (const std::string &s : imports) {
        image.undefinedSymbols.push_back(std::string(kImportPrefix) + s);
    }
    image.exportedSymbols = {"MissionSpecEntry", "ScriptEntry"};
    return image;
}

const tMissionSpec *fakeMissionSpecEntry(float) { return nullptr; }
tScriptEntry *fakeScriptEntry() { return nullptr; }

ModuleLoaderMock mockWith(const std::string &name, ModuleImage image) {
    ModuleLoaderMock loader(committedAllowList(), AllowList::parse("memcpy\n"));
    loader.addModule("modules/" + name + "/" + name + ".dylib", std::move(image),
                     fakeMissionSpecEntry, fakeScriptEntry);
    return loader;
}

std::string refusalFor(const std::string &name, ModuleImage image) {
    ModuleLoaderMock loader = mockWith(name, std::move(image));
    std::optional<LoadError> error = loader.verify("modules/" + name + "/" + name + ".dylib");
    REQUIRE(error.has_value());
    return describe(*error);
}

} // namespace

TEST_CASE("a module that imports only allowed symbols loads in the mock, with its entry points") {
    ModuleLoaderMock loader =
        mockWith("Conforming", imageOf("Conforming", readImports("conforming.imports")));
    const std::filesystem::path path = "modules/Conforming/Conforming.dylib";

    CHECK_FALSE(loader.verify(path).has_value());
    LoadResult result = loader.load(path);
    REQUIRE(result.ok());
    CHECK(result.module.id != 0);
    CHECK(result.module.manifest.name == "Conforming");
    CHECK(result.module.missionSpecEntry == &fakeMissionSpecEntry);
    CHECK(result.module.scriptEntry == &fakeScriptEntry);
    CHECK(loader.isLoaded(path));

    CHECK(loader.load(path).module.id == result.module.id);
    loader.unload(result.module.id);
    CHECK_FALSE(loader.isLoaded(path));
}

TEST_CASE("a module with a foreign import is refused, naming the first foreign import") {
    ModuleLoaderMock loader =
        mockWith("Foreign", imageOf("Foreign", readImports("foreign.imports")));
    const std::filesystem::path path = "modules/Foreign/Foreign.dylib";

    LoadResult result = loader.load(path);
    REQUIRE_FALSE(result.ok());
    CHECK(result.error->reason == Refusal::kForeignImport);
    CHECK(result.error->detail == "KERNEL32.DLL!CreateProcessA");
    CHECK(describe(*result.error) ==
          "refused: Foreign: imports KERNEL32.DLL!CreateProcessA, which is outside the scripting "
          "library and the C runtime");
    CHECK_FALSE(loader.isLoaded(path));
}

TEST_CASE("the import check alone names the first foreign import in import order") {
    std::optional<LoadError> error =
        checkImports("Foreign", readImports("foreign.imports"), committedAllowList());
    REQUIRE(error.has_value());
    CHECK(error->detail == "KERNEL32.DLL!CreateProcessA");
    CHECK_FALSE(
        checkImports("Conforming", readImports("conforming.imports"), committedAllowList()));
}

TEST_CASE("a host import passes only when it is on the host baseline list") {
    ModuleImage image = imageOf("HostRuntime", {"_atexit"});
    image.undefinedSymbols.push_back("memcpy");
    CHECK_FALSE(mockWith("HostRuntime", image).verify("modules/HostRuntime/HostRuntime.dylib"));

    image.undefinedSymbols.push_back("system");
    CHECK(refusalFor("HostRuntime", image) ==
          "refused: HostRuntime: imports system, which is outside the scripting library and the "
          "C runtime");
}

TEST_CASE("a library import is checked by the original's name, not the host's") {
    // `_free` is on the allow-list as the original's C runtime free; the same name without the
    // prefix is a host symbol, and the host baseline does not hold it.
    ModuleImage image = imageOf("Names", {"_free"});
    CHECK_FALSE(mockWith("Names", image).verify("modules/Names/Names.dylib"));
    image.undefinedSymbols.push_back("_free");
    CHECK(refusalFor("Names", image) ==
          "refused: Names: imports _free, which is outside the scripting library and the C "
          "runtime");
}

TEST_CASE("each refusal of the contract has its message") {
    const std::filesystem::path missing = "modules/Missing/Missing.dylib";
    ModuleLoaderMock empty(committedAllowList(), AllowList());
    REQUIRE(empty.verify(missing).has_value());
    CHECK(describe(*empty.verify(missing)) == "refused: Missing: not found");

    ModuleImage noManifest = imageOf("M", {});
    noManifest.manifest.reset();
    CHECK(refusalFor("M", noManifest) == "refused: M: no module.json beside the library");

    ModuleImage badManifest = imageOf("M", {});
    badManifest.manifest.reset();
    badManifest.manifestError = "source_sha256";
    CHECK(refusalFor("M", badManifest) == "refused: M: module.json is invalid: source_sha256");

    ModuleImage wrongName = imageOf("M", {});
    wrongName.manifest->name = "Other";
    CHECK(refusalFor("M", wrongName) ==
          "refused: M: module.json is invalid: name Other, expected M");

    ModuleImage notLoadable = imageOf("M", {});
    notLoadable.notLoadable = "not a Mach-O file";
    CHECK(refusalFor("M", notLoadable) ==
          "refused: M: not a loadable module for this host: not a Mach-O file");

    ModuleImage noTag = imageOf("M", {});
    noTag.abiTag.reset();
    CHECK(refusalFor("M", noTag) == "refused: M: no sfcop_module_abi version tag");

    ModuleImage noScriptEntry = imageOf("M", {});
    noScriptEntry.exportedSymbols = {"MissionSpecEntry"};
    CHECK(refusalFor("M", noScriptEntry) == "refused: M: does not export ScriptEntry");

    ModuleImage noEntries = imageOf("M", {});
    noEntries.exportedSymbols.clear();
    CHECK(refusalFor("M", noEntries) == "refused: M: does not export MissionSpecEntry");
}

TEST_CASE("a version tag that differs in any field is refused, naming the field") {
    ModuleImage magic = imageOf("M", {});
    magic.abiTag->magic[7] = 'X';
    CHECK(refusalFor("M", magic) ==
          "refused: M: version tag mismatch: magic SFCOPMOX, expected SFCOPMOD");
    magic.abiTag->magic[7] = '\0';
    CHECK(refusalFor("M", magic) ==
          "refused: M: version tag mismatch: magic SFCOPMO\\x00, expected SFCOPMOD");

    ModuleImage contract = imageOf("M", {});
    contract.abiTag->contractVersion = 2;
    CHECK(refusalFor("M", contract) ==
          "refused: M: version tag mismatch: contract version 2, expected 1");

    ModuleImage script = imageOf("M", {});
    script.abiTag->scriptVersion = 21.0f;
    CHECK(refusalFor("M", script) ==
          "refused: M: version tag mismatch: scripting library version 21.0, expected 22.0");

    ModuleImage abi = imageOf("M", {});
    std::memset(abi.abiTag->compilerAbi, 0, sizeof abi.abiTag->compilerAbi);
    std::memcpy(abi.abiTag->compilerAbi, "mips-irix-sgi", 13);
    CHECK(refusalFor("M", abi) ==
          "refused: M: version tag mismatch: compiler ABI mips-irix-sgi, expected " +
              std::string(kHostAbiTag));
}

TEST_CASE("a dynamic lookup of a foreign name is refused like a direct import") {
    ModuleLoaderMock loader =
        mockWith("Lookup", imageOf("Lookup", readImports("conforming.imports")));
    const LoadResult loaded = loader.load("modules/Lookup/Lookup.dylib");
    REQUIRE(loaded.ok());
    const ModuleId id = loaded.module.id;

    // GetProcAddress of a name on the allow-list, the module name as a program might spell it.
    CHECK_FALSE(loader.checkLookup(id, "kernel32", "HeapAlloc"));
    CHECK_FALSE(loader.checkLookup(id, "C:\\WINDOWS\\system32\\KERNEL32.dll", "HeapAlloc"));

    // GetProcAddress of a name that is not.
    std::optional<LoadError> error = loader.checkLookup(id, "kernel32.dll", "CreateProcessA");
    REQUIRE(error.has_value());
    CHECK(error->reason == Refusal::kForeignImport);
    CHECK(describe(*error) == "refused: Lookup: imports KERNEL32.DLL!CreateProcessA, which is "
                              "outside the scripting library and the C runtime");

    // By ordinal: no ordinal is on the allow-list.
    error = loader.checkLookup(id, "KERNEL32", "#5");
    REQUIRE(error.has_value());
    CHECK(error->detail == "KERNEL32.DLL!#5");

    // LoadLibraryA / GetModuleHandleA: only a module the allow-list names.
    CHECK_FALSE(loader.checkLookup(id, "user32", ""));
    error = loader.checkLookup(id, "C:/WINDOWS/system32/wsock32.dll", "");
    REQUIRE(error.has_value());
    CHECK(describe(*error) == "refused: Lookup: imports WSOCK32.DLL, which is outside the "
                              "scripting library and the C runtime");
    error = loader.checkLookup(id, "WSOCK32", "connect");
    REQUIRE(error.has_value());
    CHECK(error->detail == "WSOCK32.DLL!connect");

    // The module itself: only its own exports.
    CHECK_FALSE(loader.checkLookup(id, "", ""));
    CHECK_FALSE(loader.checkLookup(id, "", "ScriptEntry"));
    error = loader.checkLookup(id, "", "system");
    REQUIRE(error.has_value());
    CHECK(error->detail == "system");

    loader.unload(id);
    error = loader.checkLookup(id, "kernel32", "HeapAlloc");
    REQUIRE(error.has_value());
    CHECK(error->reason == Refusal::kNotFound);
}

TEST_CASE("a Windows module name is compared upper case, without directory, with .DLL added") {
    CHECK(normalizeModuleName("kernel32") == "KERNEL32.DLL");
    CHECK(normalizeModuleName("C:\\WINDOWS\\system32\\User32.dll") == "USER32.DLL");
    CHECK(normalizeModuleName("dir/advapi32.DLL") == "ADVAPI32.DLL");
    CHECK(normalizeModuleName("MSS32.ASI") == "MSS32.ASI");
    CHECK(normalizeModuleName("") == "");
}

TEST_CASE("the checks are made in the contract's order") {
    // A module that fails the tag check and the import check is refused for its tag.
    ModuleImage image = imageOf("M", {"KERNEL32.DLL!CreateProcessA"});
    image.abiTag->contractVersion = 0;
    image.exportedSymbols.clear();
    CHECK(refusalFor("M", image) ==
          "refused: M: version tag mismatch: contract version 0, expected 1");
}

TEST_CASE("the host tag names this build's ABI") {
    const ModuleAbiTag tag = hostAbiTag();
    CHECK(std::memcmp(tag.magic, "SFCOPMOD", 8) == 0);
    CHECK(tag.contractVersion == 1u);
    CHECK(tag.scriptVersion == 22.0f);
#if defined(__APPLE__) && defined(__aarch64__)
    CHECK(kHostAbiTag == "arm64-macos-itanium");
#endif
    CHECK(std::string(tag.compilerAbi) == std::string(kHostAbiTag));
}

TEST_CASE("an allow-list skips its header and blank lines and finds symbols by exact name") {
    const AllowList list = AllowList::parse("# header\n\nb\r\na\na\n");
    CHECK(list.size() == 2);
    CHECK(list.contains("a"));
    CHECK(list.contains("b"));
    CHECK_FALSE(list.contains("# header"));
    CHECK_FALSE(list.contains("A"));
    CHECK(committedAllowList().contains("??0tShipInfo@@QAE@XZ"));
    CHECK(committedAllowList().contains("KERNEL32.DLL!HeapAlloc"));
    CHECK(committedAllowList().namesModule("KERNEL32.DLL"));
    CHECK_FALSE(committedAllowList().namesModule("KERNEL32"));
    CHECK_FALSE(committedAllowList().namesModule("WSOCK32.DLL"));
}
