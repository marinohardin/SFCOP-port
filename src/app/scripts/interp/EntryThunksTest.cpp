// The pool of entry functions: one per address while it is held; none when every one is held; one
// given back is handed out again, the longest given back first, and then enters its new address.
// The pool is the process's, so each case gives back everything it took.
#include "app/scripts/interp/EntryThunks.h"

#include "app/scripts/interp/Interpreter.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

using namespace sfcop::scripts::interp;

namespace {

constexpr std::uint32_t kFirst = 0x10000000; // invented script addresses, one per function
constexpr std::uint32_t kAfter = 0x20000000; // past every one this test takes

} // namespace

TEST_CASE("an address keeps its function until it is given back, and the pool runs out") {
    Interpreter interpreter{Machine{}};
    const std::size_t heldBefore = entryThunksInUse();
    const HostFunction first = entryThunk(interpreter, kFirst);
    REQUIRE(first != nullptr);
    CHECK(entryThunk(interpreter, kFirst) == first); // asked again: the same function
    std::vector<HostFunction> taken{first};
    for (std::uint32_t address = kFirst + 1; address < kAfter; ++address) {
        const HostFunction f = entryThunk(interpreter, address);
        if (f == nullptr) {
            break;
        }
        taken.push_back(f);
    }
    CHECK(taken.size() == kEntryThunks - heldBefore);
    CHECK(entryThunksInUse() == kEntryThunks);
    CHECK(entryThunk(interpreter, kAfter) == nullptr); // spent: the lookup answers nothing
    CHECK(entryThunk(interpreter, kFirst + 1) == taken[1]); // one held is still answered

    releaseEntryThunks(interpreter, kFirst, kAfter);
    CHECK(entryThunksInUse() == heldBefore);
}

TEST_CASE("a function given back is handed out again, the longest given back first") {
    Interpreter interpreter{Machine{}};
    // Hold every free function, so the next ones handed out are the two given back below.
    std::vector<std::uint32_t> held;
    for (std::uint32_t address = kFirst; entryThunk(interpreter, address) != nullptr; ++address) {
        held.push_back(address);
    }
    REQUIRE(held.size() >= 2u);
    const HostFunction a = entryThunk(interpreter, held[0]);
    const HostFunction b = entryThunk(interpreter, held[1]);
    releaseEntryThunks(interpreter, held[1], held[1] + 1); // b first, then a
    releaseEntryThunks(interpreter, held[0], held[0] + 1);
    // A caller that kept b or a now reaches the new addresses: the pool's documented reuse.
    CHECK(entryThunk(interpreter, kAfter) == b);
    CHECK(entryThunk(interpreter, kAfter + 1) == a);
    CHECK(entryThunk(interpreter, kAfter + 2) == nullptr);

    releaseEntryThunks(interpreter, kFirst, kAfter + 3);
}
