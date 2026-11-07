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

extern "C" {
#include "win32hle.h"
extern volatile unsigned hle_dbg_takes;
int hle_debug_waiters(void);

// What host_lite.c needs from the rest of the runtime, for a machine that runs no guest code.
const recomp_dispatch_entry_t recomp_dispatch_table[1] = {{0, nullptr}};
const uint32_t recomp_dispatch_count = 0;
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
