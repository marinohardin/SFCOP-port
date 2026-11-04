// The fault classification with hand-written address ranges.
#include "app/guest/FaultClass.h"

#include <doctest/doctest.h>

namespace sfcop::guest {

TEST_CASE("a pc is lifted code when it lies in a range, end excluded") {
    CodeRanges ranges;
    ranges.Add({0x1000, 0x2000});
    ranges.Add({0x8000, 0x8100});
    CHECK_FALSE(ranges.Contains(0xFFF));
    CHECK(ranges.Contains(0x1000));
    CHECK(ranges.Contains(0x1FFF));
    CHECK_FALSE(ranges.Contains(0x2000));
    CHECK_FALSE(ranges.Contains(0x4000));
    CHECK(ranges.Contains(0x80FF));
    CHECK_FALSE(ranges.Contains(0x8100));
}

TEST_CASE("no ranges contain nothing") {
    CodeRanges ranges;
    CHECK_FALSE(ranges.Contains(0));
    CHECK_FALSE(ranges.Contains(0x1000));
}

TEST_CASE("the span of function addresses runs from the lowest to the highest plus the margin") {
    const std::uintptr_t addresses[] = {0x5000, 0x3000, 0x9000, 0x4000};
    const CodeRange span = SpanOfFunctions(addresses, 4, 0x800);
    CHECK(span.begin == 0x3000);
    CHECK(span.end == 0x9800);
    const CodeRange none = SpanOfFunctions(nullptr, 0, 0x800);
    CHECK(none.begin == 0);
    CHECK(none.end == 0);
}

TEST_CASE("the C library's leaf memory routines are told from other host code") {
    CHECK(IsLeafMemoryRoutine("_platform_memmove"));
    CHECK(IsLeafMemoryRoutine("__memcpy_avx_unaligned_erms"));
    CHECK(IsLeafMemoryRoutine("memset"));
    CHECK(IsLeafMemoryRoutine("_platform_bzero"));
    CHECK_FALSE(IsLeafMemoryRoutine("_ZN4sfcop3hle10kernel32rt7Runtime8InstanceEv"));
    CHECK_FALSE(IsLeafMemoryRoutine("pthread_mutex_lock"));
    CHECK_FALSE(IsLeafMemoryRoutine(nullptr));
}

TEST_CASE("an access of unknown kind is recorded as a read") {
    CHECK(AccessViolationFlag(FaultAccess::kRead) == 0);
    CHECK(AccessViolationFlag(FaultAccess::kUnknown) == 0);
    CHECK(AccessViolationFlag(FaultAccess::kWrite) == 1);
}

} // namespace sfcop::guest
