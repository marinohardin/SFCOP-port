#include "hle/kernel32rt/ThreadLocal.h"

#include "platform/Threads/ThreadsMock.h"

#include <doctest/doctest.h>

namespace sfcop::hle::kernel32rt {
namespace {

using platform::Error;
using platform::ThreadsMock;

TEST_CASE("TlsAlloc gives a slot, or TLS_OUT_OF_INDEXES when none is left") {
    ThreadsMock threads;
    threads.allocateTlsSlotResult = std::uint32_t{5};
    ThreadLocal slots(threads);
    CHECK(slots.Allocate().value == 5);
    threads.allocateTlsSlotResult = Error::kOutOfResources;
    const Answer none = slots.Allocate();
    CHECK(none.value == kTlsOutOfIndexes);
    CHECK(none.lastError == kErrorNotEnoughMemory);
}

TEST_CASE("TlsGetValue clears the last error, so that a stored 0 is not a failure") {
    ThreadsMock threads;
    threads.getTlsValueResult = std::uint64_t{0};
    ThreadLocal slots(threads);
    const Answer zero = slots.Get(5);
    CHECK(zero.value == 0);
    CHECK(zero.lastError == kErrorSuccess);
    threads.getTlsValueResult = std::uint64_t{0x00C90010u};
    CHECK(slots.Get(5).value == 0x00C90010u);
}

TEST_CASE("TlsGetValue and TlsSetValue fail on a slot that is not allocated") {
    ThreadsMock threads;
    threads.getTlsValueResult = Error::kInvalidArgument;
    threads.setTlsValueResult = Error::kInvalidArgument;
    ThreadLocal slots(threads);
    CHECK(slots.Get(99).lastError == kErrorInvalidParameter);
    const Answer set = slots.Set(99, 1);
    CHECK(set.value == 0);
    CHECK(set.lastError == kErrorInvalidParameter);
}

TEST_CASE("TlsSetValue stores the guest's 32-bit value") {
    ThreadsMock threads;
    ThreadLocal slots(threads);
    CHECK(slots.Set(5, 0x00C90010u).value == 1);
    CHECK(threads.log.Count("SetTlsValue") == 1);
    CHECK(threads.log.Calls().back() == "SetTlsValue(5, 13172752)");
}

} // namespace
} // namespace sfcop::hle::kernel32rt
