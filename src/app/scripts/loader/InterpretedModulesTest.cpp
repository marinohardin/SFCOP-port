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
    CHECK(provider.BindsRow(kHostRow));
    CHECK_FALSE(provider.BindsRow(kHostRow + 0x10));
    CHECK(provider.AllowsRow(kHostRow));              // its calls out reach it
    CHECK_FALSE(provider.AllowsRow(kHostRow + 0x10)); // a row with no name here: not reached
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

TEST_CASE("when every entry function is held, the dispatch answers nothing for script code until one is given back") {
    Guest g;
    InterpretedModuleProvider provider(g.runtime, g.machine, g.reader(), heapAllocOnly());
    REQUIRE(provider.Load("Scripts\\Invented.scr") == 0x60000000u);
    REQUIRE(g.hook != nullptr);
    // Another interpreter holds every free function.
    Interpreter other{Machine{}};
    std::uint32_t taken = 0x10000000u;
    while (entryThunk(other, taken) != nullptr) {
        ++taken;
    }
    CHECK(g.hook(0x60001020u) == nullptr); // said on stderr: the game holds them all
    releaseEntryThunks(other, 0x10000000u, taken);
    CHECK(g.hook(0x60001020u) != nullptr);
}

TEST_CASE("a script's GetProcAddress answers 0 for a name the allow-list leaves out") {
    Guest g;
    InterpretedModuleProvider provider(g.runtime, g.machine, g.reader(), heapAllocOnly());
    CHECK(provider.ScriptMayLoad("KERNEL32.DLL"));      // the list names it (KERNEL32.DLL!HeapAlloc)
    CHECK_FALSE(provider.ScriptMayLoad("OSSPECIFIC98.DLL"));
    CHECK_FALSE(provider.ScriptMayLoad("WSOCK32.DLL"));
    CHECK_FALSE(provider.ScriptMayFind(0x7C800000u, "HeapAlloc")); // a handle no shim gave

    const char name[] = "IsProcessorFeaturePresent";
    std::memcpy(g.window.data() + 0x80000, name, sizeof name); // at guest 0x60080000
    g.push(0x60080000u);  // the name
    g.push(0x7C800000u);  // a module handle that is not a script's
    g.push(kLiftedReturnAddress);
    g.regs[kEax] = 0x5555u;
    provider.FindForScript();
    CHECK(g.regs[kEax] == 0u);
    CHECK(g.regs[kEsp] == kStackTop); // stdcall: the return address and both arguments popped
}

namespace {

// The lock path every shipped script's library runs, on an invented script: ScriptEntry loads the
// game's OSSpecific98 library, finds OS_InterlockedCompareExchange in it, falls back to a function
// of its own when that answers 0 (the game's throws; this one marks 0x1000231C and reports the
// lock held), stores the pointer at 0x10002314 and takes the lock at 0x10002318 through it:
// compare-exchange(&lock, 1, 0), cdecl. DllMain returns 1; MissionSpecEntry returns.
constexpr std::uint32_t kLoadRow = 0x7F002000u;   // the client's LoadLibraryA
constexpr std::uint32_t kFindRow = 0x7F002010u;   // the client's GetProcAddress
constexpr std::uint32_t kSwapRow = 0x7F002020u;   // the client's OS_InterlockedCompareExchange
constexpr std::uint32_t kFreeRow = 0x7F002030u;   // the client's FreeLibrary
constexpr std::uint32_t kOsSpecific = 0x7FF00000u; // the handle the client gives OSSpecific98

// With `freeLibrary`, it also imports FreeLibrary (slot 0x2058), which its code does not call.
std::vector<std::uint8_t> lockFile(bool freeLibrary = false) {
    test::InventedCode c;
    c.code = {
        0xB8, 0x01, 0x00, 0x00, 0x00,       // 1000 mov eax, 1
        0xC2, 0x0C, 0x00,                   // 1005 ret 12
        0xC3,                               // 1008 ret
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        0x68, 0xC0, 0x20, 0x00, 0x10,       // 1010 push 0x100020C0 ("OSSpecific98")
        0xFF, 0x15, 0x50, 0x20, 0x00, 0x10, // 1015 call [0x10002050] (LoadLibraryA)
        0xA3, 0x10, 0x23, 0x00, 0x10,       // 101B mov [0x10002310], eax
        0x68, 0xD0, 0x20, 0x00, 0x10,       // 1020 push 0x100020D0 ("OS_InterlockedCompareExchange")
        0x50,                               // 1025 push eax
        0xFF, 0x15, 0x54, 0x20, 0x00, 0x10, // 1026 call [0x10002054] (GetProcAddress)
        0x85, 0xC0,                         // 102C test eax, eax
        0x75, 0x05,                         // 102E jnz 1035
        0xB8, 0x50, 0x10, 0x00, 0x10,       // 1030 mov eax, 0x10001050 (the fallback)
        0xA3, 0x14, 0x23, 0x00, 0x10,       // 1035 mov [0x10002314], eax
        0x6A, 0x00,                         // 103A push 0 (comparand)
        0x6A, 0x01,                         // 103C push 1 (exchange)
        0x68, 0x18, 0x23, 0x00, 0x10,       // 103E push 0x10002318 (the lock)
        0xFF, 0x15, 0x14, 0x23, 0x00, 0x10, // 1043 call [0x10002314]
        0x83, 0xC4, 0x0C,                   // 1049 add esp, 12
        0xC3,                               // 104C ret
        0x90, 0x90, 0x90,
        0xC7, 0x05, 0x1C, 0x23, 0x00, 0x10, 0x01, 0x00, 0x00, 0x00, // 1050 mov [0x1000231C], 1
        0xB8, 0x01, 0x00, 0x00, 0x00,       // 105A mov eax, 1
        0xC3,                               // 105F ret
    };
    c.entry = 0x1000;
    c.missionSpecEntry = 0x1008;
    c.scriptEntry = 0x1010;
    c.relocations = {0x1011, 0x1017, 0x101C, 0x1021, 0x1028, 0x1031, 0x1036, 0x103F, 0x1045, 0x1052};
    test::Pe pe = test::invented(c);
    // Its imports: kernel32.dll's LoadLibraryA (slot 0x2050) and GetProcAddress (slot 0x2054).
    for (std::uint32_t table : {0x2040u, 0x2050u}) {
        pe.data32(table, 0x2080);
        pe.data32(table + 4, 0x20A0);
        pe.data32(table + 8, 0);
    }
    pe.dataText(0x2082, "LoadLibraryA");
    pe.dataText(0x20A2, "GetProcAddress");
    pe.dataText(0x20C0, "OSSpecific98");
    pe.dataText(0x20D0, "OS_InterlockedCompareExchange");
    if (freeLibrary) {
        for (std::uint32_t table : {0x2040u, 0x2050u}) {
            pe.data32(table + 8, 0x20F0);
            pe.data32(table + 12, 0);
        }
        pe.dataText(0x20F2, "FreeLibrary");
    }
    return pe.bytes;
}

struct LockGuest : Guest {
    int loads = 0, finds = 0, swaps = 0, frees = 0;
    std::uint32_t loadName = 0, findName = 0; // where the client read the names it was handed

    LockGuest() {
        file = lockFile();
        runtime.importAddress = [](const std::string &import) {
            if (import == "KERNEL32.DLL!LoadLibraryA") {
                return kLoadRow;
            }
            if (import == "KERNEL32.DLL!FreeLibrary") {
                return kFreeRow;
            }
            CHECK(import == "KERNEL32.DLL!GetProcAddress");
            return kFindRow;
        };
        machine.lookup = [](std::uint32_t address) -> HostFunction {
            if (const HostFunction shim = InterpretedModuleProvider::ShimAt(address)) {
                return shim;
            }
            switch (address) {
            case kLoadRow: return &load;
            case kFindRow: return &find;
            case kSwapRow: return &swap;
            case kFreeRow: return &freeLibrary;
            default: return nullptr;
            }
        };
    }
    static LockGuest &self() { return *static_cast<LockGuest *>(g_guest); }
    std::string text(std::uint32_t address) const {
        return reinterpret_cast<const char *>(window.data() + (address - kWindow));
    }
    std::uint32_t arg(unsigned n) const { return at32(regs[kEsp] + 4 * n); }

    // LoadLibraryA(name), stdcall: the OSSpecific library, else 0. The shim hands it the name it
    // checked, normalised, so the script's own spelling never reaches it.
    static void load() {
        LockGuest &g = self();
        ++g.loads;
        g.loadName = g.arg(1);
        g.regs[kEax] = g.text(g.arg(1)) == "OSSPECIFIC98.DLL" ? kOsSpecific : 0u;
        g.regs[kEsp] += 8;
    }
    // GetProcAddress(module, name), stdcall: the compare-exchange in the OSSpecific library.
    static void find() {
        LockGuest &g = self();
        ++g.finds;
        g.findName = g.arg(2);
        g.regs[kEax] = g.arg(1) == kOsSpecific && g.text(g.arg(2)) == "OS_InterlockedCompareExchange" ? kSwapRow : 0u;
        g.regs[kEsp] += 12;
    }
    // FreeLibrary(module), stdcall: succeeds for the OSSpecific library only.
    static void freeLibrary() {
        LockGuest &g = self();
        ++g.frees;
        g.regs[kEax] = g.arg(1) == kOsSpecific ? 1u : 0u;
        g.regs[kEsp] += 8;
    }
    // OS_InterlockedCompareExchange(destination, exchange, comparand), cdecl.
    static void swap() {
        LockGuest &g = self();
        ++g.swaps;
        const std::uint32_t destination = g.arg(1), old = g.at32(destination);
        if (old == g.arg(3)) {
            const std::uint32_t exchange = g.arg(2);
            std::memcpy(g.window.data() + (destination - kWindow), &exchange, 4);
        }
        g.regs[kEax] = old;
        g.regs[kEsp] += 4;
    }
};

const AllowList &lockList(bool osSpecific) {
    static const AllowList with = AllowList::parse("KERNEL32.DLL!GetProcAddress\nKERNEL32.DLL!LoadLibraryA\n"
                                                   "OSSPECIFIC98.DLL!OS_InterlockedCompareExchange\n");
    static const AllowList without = AllowList::parse("KERNEL32.DLL!GetProcAddress\nKERNEL32.DLL!LoadLibraryA\n");
    return osSpecific ? with : without;
}

} // namespace

TEST_CASE("the lock path finds the game's compare-exchange and takes the lock, rather than throwing") {
    LockGuest g;
    InterpretedModuleProvider provider(g.runtime, g.machine, g.reader(), lockList(true));
    CHECK(provider.ScriptMayLoad("OSSPECIFIC98.DLL"));
    REQUIRE(provider.Load("Scripts\\Invented.scr") == 0x60000000u);
    CHECK(g.at32(0x60002050u) == 0x70000000u); // LoadLibraryA bound to its shim
    CHECK(g.at32(0x60002054u) == 0x70000003u); // GetProcAddress likewise

    CHECK(provider.Call(0x60001010u, 0, nullptr) == 0u); // the lock was free
    CHECK(g.loads == 1);
    CHECK(g.finds == 1);
    CHECK(g.swaps == 1);
    CHECK(g.at32(0x60002310u) == kOsSpecific);
    CHECK(g.at32(0x60002314u) == kSwapRow);
    CHECK(g.at32(0x60002318u) == 1u); // taken
    CHECK(g.at32(0x6000231Cu) == 0u); // the fallback never ran
    // The client read copies on the stack, not the script's strings at 0x600020C0 and 0x600020D0.
    CHECK(g.loadName < kStackTop);
    CHECK(g.loadName > kStackTop - 0x100u);
    CHECK(g.findName < kStackTop);
    CHECK(g.findName > kStackTop - 0x100u);
    CHECK(provider.AllowsRow(kSwapRow)); // the row it was given, its call out reaches
    CHECK(provider.ScriptMayFind(kOsSpecific, "OS_InterlockedCompareExchange"));
    CHECK_FALSE(provider.ScriptMayFind(kOsSpecific, "OS_Other"));
    CHECK(g.regs[kEsp] == kStackTop);
}

namespace {

// Calls LoadLibraryA's shim with `name`, written at guest 0x60080000, as the script would.
std::uint32_t loadAsScript(LockGuest &g, InterpretedModuleProvider &provider, const std::string &name) {
    std::memcpy(g.window.data() + 0x80000, name.c_str(), name.size() + 1);
    g.push(0x60080000u);
    g.push(kLiftedReturnAddress);
    g.regs[kEax] = 0x5555u;
    provider.LoadForScript(0, 4);
    CHECK(g.regs[kEsp] == kStackTop); // stdcall: the return address and the name popped
    return g.regs[kEax];
}

} // namespace

TEST_CASE("a module lookup checks the name the client is handed: a long or pathed name is refused") {
    LockGuest g;
    InterpretedModuleProvider provider(g.runtime, g.machine, g.reader(), lockList(true));
    REQUIRE(provider.Load("Scripts\\Invented.scr") == 0x60000000u);

    // A name longer than the shim reads, whose first kLookupNameLimit bytes end in an allowed module.
    std::string longName(InterpretedModuleProvider::kLookupNameLimit - std::string("\\OSSpecific98").size(), 'x');
    longName += "\\OSSpecific98";
    REQUIRE(longName.size() == InterpretedModuleProvider::kLookupNameLimit);
    CHECK(loadAsScript(g, provider, longName + "x.scr") == 0u);
    CHECK(loadAsScript(g, provider, std::string(300, 'A')) == 0u);
    // An allowed module behind a path, after one, or after a drive.
    CHECK(loadAsScript(g, provider, "C:\\mods\\OSSpecific98.dll") == 0u);
    CHECK(loadAsScript(g, provider, "mods/OSSpecific98") == 0u);
    CHECK(loadAsScript(g, provider, "OSSpecific98.dll\\..\\OSSpecific98.dll") == 0u);
    CHECK(loadAsScript(g, provider, "C:OSSpecific98") == 0u);
    CHECK(loadAsScript(g, provider, "") == 0u);
    CHECK(g.loads == 0); // the client was never asked

    // The name the stock scripts pass, and the same module spelled otherwise.
    CHECK(loadAsScript(g, provider, "OSSpecific98") == kOsSpecific);
    CHECK(loadAsScript(g, provider, "osspecific98.DLL") == kOsSpecific);
    CHECK(g.loads == 2);
    CHECK(g.text(g.loadName) == "OSSPECIFIC98.DLL");
    std::string limit(InterpretedModuleProvider::kLookupNameLimit, 'y');
    CHECK(loadAsScript(g, provider, limit) == 0u); // read whole, and not on the list
    CHECK(g.loads == 2);
}

TEST_CASE("a function lookup refuses a name longer than the shim reads and hands the client its copy") {
    LockGuest g;
    InterpretedModuleProvider provider(g.runtime, g.machine, g.reader(), lockList(true));
    REQUIRE(provider.Load("Scripts\\Invented.scr") == 0x60000000u);
    REQUIRE(loadAsScript(g, provider, "OSSpecific98") == kOsSpecific);

    const auto find = [&](const std::string &name) {
        std::memcpy(g.window.data() + 0x80000, name.c_str(), name.size() + 1);
        g.push(0x60080000u);
        g.push(kOsSpecific);
        g.push(kLiftedReturnAddress);
        g.regs[kEax] = 0x5555u;
        provider.FindForScript();
        CHECK(g.regs[kEsp] == kStackTop);
        return g.regs[kEax];
    };
    CHECK(find("OS_InterlockedCompareExchange" + std::string(300, 'z')) == 0u);
    CHECK(find("") == 0u);
    CHECK(g.finds == 0);
    CHECK(find("OS_InterlockedCompareExchange") == kSwapRow);
    CHECK(g.finds == 1);
    CHECK(g.findName != 0x60080000u); // a copy, which the script cannot change under the client
    CHECK(g.text(g.findName) == "OS_InterlockedCompareExchange");
}

TEST_CASE("a handle forgets its names once the script frees every reference it took") {
    LockGuest g;
    g.file = lockFile(true);
    static const AllowList withFree = AllowList::parse("KERNEL32.DLL!FreeLibrary\nKERNEL32.DLL!GetProcAddress\n"
                                                       "KERNEL32.DLL!LoadLibraryA\n"
                                                       "OSSPECIFIC98.DLL!OS_InterlockedCompareExchange\n");
    InterpretedModuleProvider provider(g.runtime, g.machine, g.reader(), withFree);
    REQUIRE(provider.Load("Scripts\\Invented.scr") == 0x60000000u);
    CHECK(g.at32(0x60002058u) == 0x70000004u); // FreeLibrary bound to its shim

    const auto freeAsScript = [&](std::uint32_t handle) {
        g.push(handle);
        g.push(kLiftedReturnAddress);
        g.regs[kEax] = 0x5555u;
        provider.FreeForScript();
        CHECK(g.regs[kEsp] == kStackTop); // stdcall: the return address and the handle popped
        return g.regs[kEax];
    };
    REQUIRE(loadAsScript(g, provider, "OSSpecific98") == kOsSpecific);
    REQUIRE(loadAsScript(g, provider, "OSSpecific98") == kOsSpecific);
    CHECK(provider.ScriptMayFind(kOsSpecific, "OS_InterlockedCompareExchange"));

    CHECK(freeAsScript(0x12340000u) == 0u); // a handle the client does not know: passed on, refused there
    CHECK(freeAsScript(kOsSpecific) == 1u);
    CHECK(provider.ScriptMayFind(kOsSpecific, "OS_InterlockedCompareExchange")); // one reference left
    CHECK(freeAsScript(kOsSpecific) == 1u);
    CHECK(g.frees == 3); // every call reached the client
    // The handle is the client's to give to another module now: the names it had allow nothing.
    CHECK_FALSE(provider.ScriptMayFind(kOsSpecific, "OS_InterlockedCompareExchange"));
}

TEST_CASE("without the OSSpecific entries the lookups answer 0 and the script's own fallback runs") {
    LockGuest g;
    InterpretedModuleProvider provider(g.runtime, g.machine, g.reader(), lockList(false));
    REQUIRE(provider.Load("Scripts\\Invented.scr") == 0x60000000u);
    CHECK(provider.Call(0x60001010u, 0, nullptr) == 1u);
    CHECK(g.loads == 0); // the client was never asked
    CHECK(g.finds == 0);
    CHECK(g.swaps == 0);
    CHECK(g.at32(0x60002310u) == 0u);
    CHECK(g.at32(0x60002314u) == 0x60001050u); // the fallback, relocated
    CHECK(g.at32(0x60002318u) == 0u);
    CHECK(g.at32(0x6000231Cu) == 1u);
    CHECK_FALSE(provider.AllowsRow(kSwapRow));
    CHECK(g.regs[kEsp] == kStackTop);
}

namespace {

// An invented script that passes a long text through two calls out, as a mission passes its
// briefing: ScriptEntry pushes the text's address and calls its HeapAlloc import (an allow-listed
// row, stdcall), then the game's code at 0x00401000 (cdecl). Each callee reads the text itself.
constexpr std::uint32_t kGameCode = 0x00401000u;
constexpr std::uint32_t kLongText = 0x60080000u;
constexpr std::size_t kLongTextSize = 8192;

std::vector<std::uint8_t> longTextFile() {
    test::InventedCode c;
    c.code = {
        0xB8, 0x01, 0x00, 0x00, 0x00,       // 1000 mov eax, 1
        0xC2, 0x0C, 0x00,                   // 1005 ret 12
        0xC3,                               // 1008 ret
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        0x68, 0x00, 0x00, 0x08, 0x60,       // 1010 push 0x60080000 (the text)
        0xFF, 0x15, 0x50, 0x20, 0x00, 0x10, // 1015 call [0x10002050] (its import)
        0x68, 0x00, 0x00, 0x08, 0x60,       // 101B push 0x60080000
        0xB8, 0x00, 0x10, 0x40, 0x00,       // 1020 mov eax, 0x00401000 (the game's code)
        0xFF, 0xD0,                         // 1025 call eax
        0x83, 0xC4, 0x04,                   // 1027 add esp, 4
        0xC3,                               // 102A ret
    };
    c.entry = 0x1000;
    c.missionSpecEntry = 0x1008;
    c.scriptEntry = 0x1010;
    c.relocations = {0x1017};
    test::Pe pe = test::invented(c);
    for (std::uint32_t table : {0x2040u, 0x2050u}) { // HeapAlloc alone, which the list allows
        pe.data32(table + 4, 0);
    }
    return pe.bytes;
}

struct TextGuest : Guest {
    std::vector<std::string> seen; // the text each callee read, from the pointer it was passed

    TextGuest() {
        file = longTextFile();
        machine.lookup = [](std::uint32_t address) -> HostFunction {
            switch (address) {
            case kHostRow: return &importRow;
            case kGameCode: return &gameCode;
            default: return nullptr;
            }
        };
    }
    static TextGuest &self() { return *static_cast<TextGuest *>(g_guest); }
    void read() {
        const std::uint32_t text = at32(regs[kEsp] + 4);
        seen.emplace_back(reinterpret_cast<const char *>(window.data() + (text - kWindow)));
    }
    static void importRow() { // stdcall, one argument
        TextGuest &g = self();
        g.read();
        g.regs[kEsp] += 8;
    }
    static void gameCode() { // cdecl
        TextGuest &g = self();
        g.read();
        g.regs[kEsp] += 4;
    }
};

} // namespace

TEST_CASE("a long text a script passes to the game's code or an import reaches the callee whole") {
    TextGuest g;
    std::string text(kLongTextSize, '\0');
    for (std::size_t i = 0; i < text.size(); ++i) {
        text[i] = static_cast<char>(1 + i % 255); // every byte value but the terminator
    }
    std::memcpy(g.window.data() + (kLongText - kWindow), text.c_str(), text.size() + 1);
    InterpretedModuleProvider provider(g.runtime, g.machine, g.reader(), heapAllocOnly());
    REQUIRE(provider.Load("Scripts\\Invented.scr") == 0x60000000u);

    provider.Call(0x60001010u, 0, nullptr);
    REQUIRE(g.seen.size() == 2);
    CHECK(g.seen[0].size() == kLongTextSize); // the import row
    CHECK(g.seen[0] == text);
    CHECK(g.seen[1].size() == kLongTextSize); // the game's code
    CHECK(g.seen[1] == text);
    CHECK(std::memcmp(g.window.data() + (kLongText - kWindow), text.c_str(), text.size() + 1) == 0); // untouched
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
