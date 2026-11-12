// The provider that interprets a .scr, on a guest machine of the test's own: an invented script is
// checked against the allow-list before anything is written, laid out at its slot with its import
// bound, attached by running its DLL entry point on the interpreter, entered through the dispatch
// hook as lifted code would enter it, and detached and released at the last FreeLibrary.
#include "app/scripts/loader/InterpretedModules.h"

#include "app/scripts/ModuleLoaderMock.h"
#include "app/scripts/interp/EntryThunks.h"
#include "app/scripts/interp/InventedScript.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using namespace sfcop::scripts;
using namespace sfcop::scripts::interp;

namespace {

constexpr std::uint32_t kWindow = 0x60000000u; // the guest memory the test backs: one MB from here
constexpr std::uint32_t kStackTop = 0x600F0000u;
constexpr std::uint32_t kHostRow = 0x7F001000u; // the client's row for KERNEL32.DLL!HeapAlloc

// The script: DllMain stores its reason at 0x10002300, calls through its HeapAlloc slot and
// stores what that returned at 0x10002304, and returns `attach` (stdcall, three arguments);
// MissionSpecEntry stores its argument at 0x10002308 and returns 0x10002010; ScriptEntry returns
// 42. Every absolute address is relocated.
test::InventedCode scriptCode(std::uint8_t attach = 1) {
    test::InventedCode c;
    c.code = {
        0x8B, 0x44, 0x24, 0x08,             // 1000 mov eax, [esp+8]
        0xA3, 0x00, 0x23, 0x00, 0x10,       // 1004 mov [0x10002300], eax
        0xFF, 0x15, 0x50, 0x20, 0x00, 0x10, // 1009 call [0x10002050]
        0xA3, 0x04, 0x23, 0x00, 0x10,       // 100F mov [0x10002304], eax
        0xB8, attach, 0x00, 0x00, 0x00,     // 1014 mov eax, attach
        0xC2, 0x0C, 0x00,                   // 1019 ret 12
        0x90, 0x90, 0x90, 0x90,             // 101C
        0x8B, 0x44, 0x24, 0x04,             // 1020 mov eax, [esp+4]
        0xA3, 0x08, 0x23, 0x00, 0x10,       // 1024 mov [0x10002308], eax
        0xB8, 0x10, 0x20, 0x00, 0x10,       // 1029 mov eax, 0x10002010
        0xC3,                               // 102E ret
        0x90,                               // 102F
        0xB8, 0x2A, 0x00, 0x00, 0x00,       // 1030 mov eax, 42
        0xC3,                               // 1035 ret
    };
    c.entry = 0x1000;
    c.missionSpecEntry = 0x1020;
    c.scriptEntry = 0x1030;
    c.relocations = {0x1005, 0x100B, 0x1010, 0x1025, 0x102A};
    return c;
}

// The script's file: the invented DLL with only its HeapAlloc import (its ordinal import, which the
// allow-list cannot hold, taken out).
std::vector<std::uint8_t> scriptFile(std::uint8_t attach = 1) {
    test::Pe pe = test::invented(scriptCode(attach));
    pe.data32(0x2044, 0);
    pe.data32(0x2054, 0);
    return pe.bytes;
}

struct Guest;
Guest *g_guest = nullptr;

struct Guest {
    std::uint32_t regs[8] = {};
    std::uint32_t kind = 0, a = 0, b = 0, carry = 0;
    double st[8] = {};
    std::int64_t exact[8] = {};
    int top = 0;
    std::uint16_t cw = 0x027F;
    std::uint32_t fs = 0, gs = 0, current = 0;
    std::vector<std::uint8_t> window = std::vector<std::uint8_t>(0x100000, 0);
    Machine machine;
    GuestRuntime runtime;
    sfcop_guest_fn (*hook)(std::uint32_t) = nullptr;

    std::vector<std::pair<std::uint32_t, std::uint32_t>> reserved, released;
    int hostCalls = 0;
    std::vector<std::uint8_t> file = scriptFile();
    std::uint32_t base = kWindow;
    int reads = 0;

    Guest() {
        for (unsigned n = 0; n < 8; ++n) {
            machine.regs[n] = &regs[n];
        }
        machine.flagKind = &kind;
        machine.flagA = &a;
        machine.flagB = &b;
        machine.flagCarry = &carry;
        machine.st = st;
        machine.stExact = exact;
        machine.fpTop = &top;
        machine.fpuControl = &cw;
        machine.fsBase = &fs;
        machine.gsBase = &gs;
        machine.currentFunction = &current;
        machine.memory = reinterpret_cast<std::uint8_t *>(reinterpret_cast<std::uintptr_t>(window.data()) - kWindow);
        machine.lookup = [](std::uint32_t address) -> HostFunction { return address == kHostRow ? &host : nullptr; };
        machine.fatal = [](const char *message) { throw std::runtime_error(message); };
        runtime.reserve = [this](std::uint32_t address, std::uint32_t size) {
            reserved.emplace_back(address, size);
            return true;
        };
        runtime.release = [this](std::uint32_t address, std::uint32_t size) { released.emplace_back(address, size); };
        runtime.memory = [this](std::uint32_t address) { return window.data() + (address - kWindow); };
        runtime.importAddress = [](const std::string &import) {
            CHECK(import == "KERNEL32.DLL!HeapAlloc");
            return kHostRow;
        };
        runtime.lookupHook = &hook;
        regs[kEsp] = kStackTop;
        g_guest = this;
    }

    // A lifted function: pops the call's return address, eax = 0x1234.
    static void host() {
        Guest &g = *g_guest;
        ++g.hostCalls;
        g.regs[kEsp] += 4;
        g.regs[kEax] = 0x1234;
    }

    std::uint32_t at32(std::uint32_t address) const {
        std::uint32_t v;
        std::memcpy(&v, window.data() + (address - kWindow), 4);
        return v;
    }
    void push(std::uint32_t v) {
        regs[kEsp] -= 4;
        std::memcpy(window.data() + (regs[kEsp] - kWindow), &v, 4);
    }

    ScriptReader reader() {
        return [this](std::string_view name, std::string &why) -> std::optional<ScriptFile> {
            ++reads;
            CHECK(name == "Scripts\\Invented.scr");
            if (file.empty()) {
                why = "the file cannot be opened";
                return std::nullopt;
            }
            return ScriptFile{file, base};
        };
    }
};

const AllowList &heapAllocOnly() {
    static const AllowList list = AllowList::parse("KERNEL32.DLL!HeapAlloc\n");
    return list;
}

} // namespace

TEST_CASE("an interpreted script is laid out at its slot, bound, attached and entered through the dispatch") {
    Guest g;
    InterpretedModuleProvider provider(g.runtime, g.machine, g.reader(), heapAllocOnly());
    CHECK(provider.Handles("Scripts\\Invented.scr"));
    CHECK(provider.Handles("x.SCR"));
    CHECK_FALSE(provider.Handles("Invented.dll"));

    const std::uint32_t handle = provider.Load("Scripts\\Invented.scr");
    REQUIRE(handle == 0x60000000u);
    CHECK(provider.Owns(handle));
    CHECK(g.reserved == std::vector<std::pair<std::uint32_t, std::uint32_t>>{{0x60000000u, 0x10000u}});
    CHECK(g.at32(0x60002050u) == kHostRow);    // the import slot, bound
    CHECK(g.at32(0x60002300u) == 1u);          // DllMain saw DLL_PROCESS_ATTACH
    CHECK(g.at32(0x60002304u) == 0x1234u);     // and the host row's answer
    CHECK(g.hostCalls == 1);
    CHECK(g.regs[kEsp] == kStackTop);          // the machine's registers as they were
    CHECK(provider.Export(handle, "MissionSpecEntry") == 0x60001020u);
    CHECK(provider.Export(handle, "ScriptEntry") == 0x60001030u);
    CHECK(provider.Export(handle, "Other") == 0u);

    // The game calls MissionSpecEntry(22.0f) through the dispatch, as lifted code calls a pointer.
    REQUIRE(g.hook != nullptr);
    const sfcop_guest_fn spec = g.hook(0x60001020u);
    REQUIRE(spec != nullptr);
    CHECK(g.hook(0x60001020u) == spec);
    CHECK(g.hook(0x60001030u) != spec);
    CHECK(g.hook(0x60002000u) == nullptr); // data, not code
    CHECK(g.hook(0x70000000u) == nullptr);
    g.push(0x41B00000u);
    g.push(kLiftedReturnAddress);
    spec();
    CHECK(g.regs[kEax] == 0x60002010u);
    CHECK(g.at32(0x60002308u) == 0x41B00000u);
    CHECK(g.regs[kEsp] == kStackTop - 4); // the return address popped; the caller drops the argument
    g.regs[kEsp] = kStackTop;

    CHECK(provider.Call(0x60001030u, 0, nullptr) == 42u);
    CHECK(g.regs[kEsp] == kStackTop);
}

TEST_CASE("an interpreted script is counted, detached at the last FreeLibrary, and its functions go") {
    Guest g;
    InterpretedModuleProvider provider(g.runtime, g.machine, g.reader(), heapAllocOnly());
    const std::uint32_t handle = provider.Load("Scripts\\Invented.scr");
    REQUIRE(handle == 0x60000000u);
    CHECK(provider.Load("Scripts\\INVENTED.scr") == handle);
    CHECK(g.reads == 1);
    const sfcop_guest_fn kept = g.hook(0x60001020u);
    REQUIRE(kept != nullptr);
    const std::size_t inUse = entryThunksInUse();

    provider.Free(handle);
    CHECK(provider.Owns(handle));
    CHECK(g.released.empty());
    provider.Free(handle);
    CHECK_FALSE(provider.Owns(handle));
    CHECK(g.at32(0x60002300u) == 0u); // DllMain saw DLL_PROCESS_DETACH
    CHECK(g.released == std::vector<std::pair<std::uint32_t, std::uint32_t>>{{0x60000000u, 0x10000u}});
    CHECK(g.hook(0x60001020u) == nullptr);
    CHECK(entryThunksInUse() == inUse - 1);
    CHECK(provider.Export(handle, "MissionSpecEntry") == 0u);

    // A caller that kept the function calls it after the unload: the call fails with eax and edx
    // 0, and only the return address is popped.
    g.regs[kEax] = 0x5555u;
    g.regs[kEdx] = 0x6666u;
    g.regs[kEbx] = 0x7777u;
    g.push(0x12345678u);
    kept();
    CHECK(g.regs[kEax] == 0u);
    CHECK(g.regs[kEdx] == 0u);
    CHECK(g.regs[kEbx] == 0x7777u);
    CHECK(g.regs[kEsp] == kStackTop);
}

TEST_CASE("an interpreted script is refused before anything is written") {
    Guest g;
    SUBCASE("an import off the allow-list") {
        static const AllowList none = AllowList::parse("KERNEL32.DLL!HeapFree\n");
        InterpretedModuleProvider provider(g.runtime, g.machine, g.reader(), none);
        CHECK(provider.Load("Scripts\\Invented.scr") == 0u);
    }
    SUBCASE("a file that cannot be read") {
        g.file.clear();
        InterpretedModuleProvider provider(g.runtime, g.machine, g.reader(), heapAllocOnly());
        CHECK(provider.Load("Scripts\\Invented.scr") == 0u);
    }
    SUBCASE("a file that is not a DLL") {
        g.file[0] = 'X';
        InterpretedModuleProvider provider(g.runtime, g.machine, g.reader(), heapAllocOnly());
        CHECK(provider.Load("Scripts\\Invented.scr") == 0u);
    }
    SUBCASE("an instruction the interpreter does not know") {
        g.file[0x200 + 0x30] = 0x0F; // ScriptEntry: syscall
        g.file[0x200 + 0x31] = 0x05;
        InterpretedModuleProvider provider(g.runtime, g.machine, g.reader(), heapAllocOnly());
        CHECK(provider.Load("Scripts\\Invented.scr") == 0u);
        CHECK(provider.Lookup(0x60001030u) == nullptr);
    }
    SUBCASE("no guest machine") {
        InterpretedModuleProvider provider(g.runtime, std::nullopt, g.reader(), heapAllocOnly());
        CHECK(provider.Load("Scripts\\Invented.scr") == 0u);
        CHECK(g.reads == 0);
    }
    CHECK(g.reserved.empty());
    CHECK(g.at32(0x60002050u) == 0u);
    CHECK(g.hostCalls == 0);
}

TEST_CASE("a script whose DLL entry point refuses to attach is unmapped") {
    Guest g;
    g.file = scriptFile(0);
    InterpretedModuleProvider provider(g.runtime, g.machine, g.reader(), heapAllocOnly());
    CHECK(provider.Load("Scripts\\Invented.scr") == 0u);
    CHECK(g.hostCalls == 1);
    CHECK(g.released == std::vector<std::pair<std::uint32_t, std::uint32_t>>{{0x60000000u, 0x10000u}});
    CHECK_FALSE(provider.Owns(0x60000000u));
    CHECK(g.hook(0x60001020u) == nullptr);
}

TEST_CASE("a script's slot is its place among its folder's .scr files in byte order") {
    const std::vector<std::string> folder{"b.scr", "a.scr", "A.scr", "notes.txt", "C.SCR"};
    // Byte order: "A.scr", "C.SCR", "a.scr", "b.scr".
    CHECK(scriptSlotIndex("A.scr", folder) == 0u);
    CHECK(scriptSlotIndex("C.SCR", folder) == 1u);
    CHECK(scriptSlotIndex("a.scr", folder) == 2u);
    CHECK(scriptSlotIndex("b.scr", folder) == 3u);
    CHECK(scriptSlotIndex("B.Scr", folder) == 3u); // matched without case when no name is exact
    CHECK_FALSE(scriptSlotIndex("notes.txt", folder).has_value());
    CHECK_FALSE(scriptSlotIndex("d.scr", folder).has_value());
    CHECK(scriptSlotBase(0) == 0x60000000u);
    CHECK(scriptSlotBase(3) == 0x60300000u);
    CHECK(scriptSlotBase(255) == 0x6FF00000u);
    CHECK(scriptSlotBase(256) == 0u);
}

TEST_CASE("a script with no native module file is interpreted") {
    Guest g;
    ModuleLoaderMock loader(heapAllocOnly(), AllowList{});
    GuestModuleProvider native(loader, g.runtime, "no-such-modules-root", heapAllocOnly());
    InterpretedModuleProvider interpreted(g.runtime, g.machine, g.reader(), heapAllocOnly());
    MissionModuleProvider provider(native, interpreted);
    CHECK(provider.Handles("Scripts\\Invented.scr"));
    const std::uint32_t handle = provider.Load("Scripts\\Invented.scr");
    CHECK(handle == 0x60000000u);
    CHECK(provider.Owns(handle));
    CHECK(interpreted.Owns(handle));
    CHECK_FALSE(native.Owns(handle));
    CHECK(provider.Export(handle, "ScriptEntry") == 0x60001030u);
    provider.Free(handle);
    CHECK_FALSE(provider.Owns(handle));
}
