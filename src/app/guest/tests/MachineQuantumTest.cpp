// The machine's hand-over rule (pcrecomp's host_lite.c), as Windows schedules a thread: one that
// runs keeps the processor until it blocks, gives it up (Sleep(0)) or uses up its quantum. An
// import is a hand-over point: while another thread waits, the running one passes through it many
// times inside its quantum and keeps the machine; at the end of the quantum, the next import
// hands it over; a yield hands it over at once. The quantum is a count of hand-over points
// (imports and back-edge hooks) since the thread took the machine, so it depends only on the code
// the guest runs, never on the host's clock.
//
// Built and run against a pcrecomp clone by run_machine_quantum.sh (README.md, "Tests"); it
// needs no lifted program.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

// Which function an indirect call ran (the dispatch cases below): 1 the generated table's
// entry, 2 the manual hook's, 3 one folded in.
static int g_ran = 0;
static void ranGenerated() { g_ran = 1; }
static void ranHook() { g_ran = 2; }
static void ranFolded() { g_ran = 3; }

extern "C" {
#include "win32hle.h"
extern volatile unsigned hle_dbg_takes;
int hle_debug_waiters(void);
extern recomp_func_t (*hle_manual_hook)(uint32_t va);

// What host_lite.c needs from the rest of the runtime, for a machine that runs no guest code.
// The generated table holds one entry, so a manual hook can answer the same address.
const recomp_dispatch_entry_t recomp_dispatch_table[1] = {{0x00400100u, ranGenerated}};
const uint32_t recomp_dispatch_count = 1;
void *hle_alloc(uint32_t bytes) { return std::calloc(1, bytes); }
uint32_t hle_va_bad(const void *) { return 0; }
uint32_t hle_vm_reserve(uint32_t, uint32_t) { return 0x10000u; }
}

namespace {

constexpr unsigned kQuantum = 8; // hand-over points; the build's own is larger

void probe() {}
const win32hle_shim kProbe[] = {{"MachineQuantumProbe", probe}, {nullptr, nullptr}};

// An import, as lifted code calls one: the shim's entry, hand-over point included.
recomp_func_t importEntry() {
    static const bool registered = win32hle_register(kProbe) == 1;
    REQUIRE(registered);
    const uint32_t va = hle_resolve("MachineQuantumProbe");
    REQUIRE(va != 0u);
    recomp_func_t entry = recomp_lookup_import(va);
    REQUIRE(entry != nullptr);
    return entry;
}

// A second thread that waits for the machine, records that it got it, and calls `import` once
// while it holds it when one is given.
struct Waiter {
    std::atomic<bool> ran{false};
    std::thread thread;
    void start(recomp_func_t import = nullptr) {
        thread = std::thread([this, import] {
            mach_enter();
            if (import) {
                import();
            }
            ran = true;
            mach_leave();
        });
        while (hle_debug_waiters() == 0) {
            std::this_thread::yield();
        }
    }
};

} // namespace

TEST_CASE("a thread inside its quantum keeps the machine at imports while another waits") {
    hle_quantum_points = kQuantum;
    recomp_func_t import = importEntry();
    mach_enter(); // a take: the quantum starts
    Waiter waiter;
    waiter.start();
    const unsigned takes = hle_dbg_takes;
    for (unsigned i = 1; i < kQuantum; i++) {
        import();
    }
    CHECK(hle_dbg_takes == takes); // seven imports: nobody else took the machine
    CHECK_FALSE(waiter.ran.load());
    import(); // the eighth: the quantum is used up
    CHECK(hle_dbg_takes == takes + 2u); // the waiter took it, then this thread again
    CHECK(waiter.ran.load());
    mach_leave();
    waiter.thread.join();
}

TEST_CASE("the quantum starts again when the thread takes the machine back") {
    hle_quantum_points = kQuantum;
    recomp_func_t import = importEntry();
    mach_enter();
    Waiter first;
    first.start();
    for (unsigned i = 0; i < kQuantum; i++) {
        import();
    }
    REQUIRE(first.ran.load());
    first.thread.join();
    Waiter second;
    second.start();
    const unsigned takes = hle_dbg_takes;
    for (unsigned i = 1; i < kQuantum; i++) {
        import();
    }
    CHECK(hle_dbg_takes == takes);
    CHECK_FALSE(second.ran.load());
    import();
    CHECK(second.ran.load());
    mach_leave();
    second.thread.join();
}

TEST_CASE("a yield hands the machine to a waiting thread at once, inside the quantum") {
    hle_quantum_points = kQuantum;
    mach_enter();
    Waiter waiter;
    waiter.start();
    hle_yield(); // Sleep(0)
    CHECK(waiter.ran.load());
    mach_leave();
    waiter.thread.join();
}

// A loop that runs RECOMP_YIELD_EVERY back edges without one import is spinning on its own (a
// lock taken by another thread, with no system call in the retry): its back-edge hook hands the
// machine over at once. A loop that also called imports keeps the machine inside its quantum.
TEST_CASE("a loop with no import hands the machine over at its back edge, inside the quantum") {
    hle_quantum_points = kQuantum;
    recomp_func_t import = importEntry();
    mach_enter();
    Waiter waiter;
    waiter.start();
    const unsigned takes = hle_dbg_takes;
    import();
    recomp_yield_hook(); // an import ran since the last hook: the quantum applies
    CHECK(hle_dbg_takes == takes);
    CHECK_FALSE(waiter.ran.load());
    recomp_yield_hook(); // no import since: a spin
    CHECK(waiter.ran.load());
    CHECK(hle_dbg_takes == takes + 2u);
    mach_leave();
    waiter.thread.join();
}

// The imports a loop is judged by are its own thread's: another thread that ran imports while
// this one was off the machine does not make this one's spin look like work.
TEST_CASE("another thread's imports do not hide a loop's spin") {
    hle_quantum_points = kQuantum;
    recomp_func_t import = importEntry();
    mach_enter();
    import();
    recomp_yield_hook(); // an import ran since the last hook: the quantum applies
    Waiter importer;
    importer.start(import);
    hle_yield(); // Sleep(0): the other thread runs its import
    REQUIRE(importer.ran.load());
    importer.thread.join();
    Waiter waiter;
    waiter.start();
    recomp_yield_hook(); // this thread ran no import since its last hook: a spin
    CHECK(waiter.ran.load());
    mach_leave();
    waiter.thread.join();
}

// ---- The dispatch lookup (cmake/lifter/patches/pcrecomp-dispatch-cache.patch) -------------------
// recomp_lookup answers every indirect call from a direct-mapped cache in front of the binary
// search; hle_dispatch_fold writes a host's entries into a copy of the table, after which indirect
// calls ask the table before the manual hook. This program's generated table holds one entry,
// 0x00400100; the cases build the rest by folding. They run in file order and share the one table
// and the fold, which cannot be undone: each says what it starts from.

namespace {

void fnA() {}
void fnB() {}
void fnC() {}
void fnD() {}
const recomp_func_t kPool[4] = {fnA, fnB, fnC, fnD};

} // namespace

// An indirect call as lifted code makes one (RECOMP_ITAIL, which asks the lookups in the same
// order as RECOMP_ICALL): which function ran, or 0 for none.
static int callIndirect(std::uint32_t va) {
    RECOMP_REGS_LOCALS
    std::uint32_t _flag_k = 0, _flag_a = 0, _flag_b = 0, _cf = 0;
    g_ran = 0;
    RECOMP_ITAIL(va);
    return g_ran;
}

// A hook that answers 0x00400100, which the table also holds, and 0x00400200, which it lacks.
static recomp_func_t hookBoth(std::uint32_t va) {
    return va == 0x00400100u || va == 0x00400200u ? ranHook : nullptr;
}
static recomp_func_t hookNothing(std::uint32_t) { return nullptr; }

TEST_CASE("the generated table answers its entry only, asked once or again from the cache") {
    for (int pass = 0; pass < 2; pass++) {
        CHECK(recomp_lookup(0x00400100u) == ranGenerated);
        CHECK(recomp_lookup(0u) == nullptr);
        CHECK(recomp_lookup(0x00401000u) == nullptr);
        CHECK(recomp_lookup(0x7F000000u) == nullptr);
    }
}

// A host that never folds (every build before this patch's host side) keeps the old order: a hook
// that supplies a function in place of a lifted one wins over the table.
TEST_CASE("before a fold, an indirect call asks the manual hook before the table") {
    REQUIRE(recomp_dispatch_folded == 0);
    hle_manual_hook = hookBoth;
    CHECK(callIndirect(0x00400100u) == 2); // in the table and the hook: the hook's
    CHECK(callIndirect(0x00400200u) == 2); // in the hook only
    CHECK(callIndirect(0x00400300u) == 0); // in neither
    hle_manual_hook = hookNothing;
    CHECK(callIndirect(0x00400100u) == 1); // the hook declines: the table's
    hle_manual_hook = nullptr;
    CHECK(callIndirect(0x00400100u) == 1);
}

TEST_CASE("folded entries are found, and a miss cached before the fold is found after it") {
    CHECK(recomp_lookup(0x00402000u) == nullptr); // cached as absent
    const recomp_dispatch_entry_t rows[] = {
        {0x00403000u, fnC}, {0x00401000u, fnA}, {0x00402000u, fnB}, // unsorted on purpose
    };
    CHECK(hle_dispatch_fold(rows, 3) == 3);
    for (int pass = 0; pass < 2; pass++) {
        CHECK(recomp_lookup(0x00401000u) == fnA);
        CHECK(recomp_lookup(0x00402000u) == fnB);
        CHECK(recomp_lookup(0x00403000u) == fnC);
        CHECK(recomp_lookup(0x00401004u) == nullptr);
        CHECK(recomp_lookup(0u) == nullptr);
    }
}

TEST_CASE("a folded entry at an address the table holds replaces it; a new one is added") {
    // From the previous case: 0x00401000 fnA, 0x00402000 fnB, 0x00403000 fnC.
    const recomp_dispatch_entry_t rows[] = {{0x00402000u, fnD}, {0x00402800u, fnA}};
    CHECK(recomp_lookup(0x00402000u) == fnB); // cached before the fold
    CHECK(hle_dispatch_fold(rows, 2) == 1);
    CHECK(recomp_lookup(0x00401000u) == fnA);
    CHECK(recomp_lookup(0x00402000u) == fnD);
    CHECK(recomp_lookup(0x00402800u) == fnA);
    CHECK(recomp_lookup(0x00403000u) == fnC);
    CHECK(hle_dispatch_fold(nullptr, 0) == 0); // nothing folded: the table stays as it is
    CHECK(recomp_lookup(0x00402000u) == fnD);
}

// 40,000 entries against a cache of 16,384 slots: most slots are shared, so answers are evicted and
// searched again. Every entry is at 0x00500000 + 16 * i with kPool[i % 4]; the addresses between
// them are absent.
TEST_CASE("every entry of a table larger than the cache is found, in any order, many times") {
    constexpr std::uint32_t kCount = 40000;
    std::vector<recomp_dispatch_entry_t> rows;
    for (std::uint32_t i = 0; i < kCount; i++) {
        rows.push_back({0x00500000u + 16u * i, kPool[i % 4]});
    }
    CHECK(hle_dispatch_fold(rows.data(), kCount) == static_cast<int>(kCount));
    std::uint32_t wrong = 0;
    for (int pass = 0; pass < 3; pass++) {
        for (std::uint32_t k = 0; k < kCount; k++) {
            const std::uint32_t i = pass == 1 ? kCount - 1 - k : (k * 7919u) % kCount;
            wrong += recomp_lookup(0x00500000u + 16u * i) != kPool[i % 4];
            wrong += recomp_lookup(0x00500000u + 16u * i + 4u) != nullptr;
        }
    }
    CHECK(wrong == 0u);
    CHECK(recomp_lookup(0x00402000u) == fnD); // the earlier entries are still there
    CHECK(recomp_lookup(0x00403000u) == fnC);
}

// The cache is read without the machine lock: a thread that reads a slot while the machine's
// holder writes it must see a whole answer, the old or the new, never half of each.
TEST_CASE("lookups on two threads at once give only right answers") {
    // From the previous case: 0x00500000 + 16 * i holds kPool[i % 4] for i below 40,000.
    std::atomic<std::uint32_t> wrong{0};
    auto run = [&wrong](std::uint32_t seed) {
        std::uint32_t bad = 0;
        for (std::uint32_t k = 0; k < 200000; k++) {
            const std::uint32_t i = (k * seed) % 40000u;
            bad += recomp_lookup(0x00500000u + 16u * i) != kPool[i % 4];
        }
        wrong += bad;
    };
    std::thread a(run, 7919u), b(run, 104729u);
    a.join();
    b.join();
    CHECK(wrong.load() == 0u);
}

// After a fold the host's replacements are in the table, so the table is asked first; the hook
// still answers what the table lacks (the game's missed constructor, a mission module's address).
TEST_CASE("after a fold, an indirect call asks the table first and the hook for what it lacks") {
    const recomp_dispatch_entry_t rows[] = {{0x00400100u, ranFolded}};
    CHECK(hle_dispatch_fold(rows, 1) == 0); // replaces the generated entry
    REQUIRE(recomp_dispatch_folded == 1);
    hle_manual_hook = hookBoth;
    CHECK(callIndirect(0x00400100u) == 3); // in the table and the hook: the folded entry
    CHECK(callIndirect(0x00400200u) == 2); // in the hook only
    CHECK(callIndirect(0x00400300u) == 0); // in neither
    hle_manual_hook = nullptr;
    CHECK(callIndirect(0x00400200u) == 0);
}
