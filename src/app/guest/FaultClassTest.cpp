// The fault classification with hand-written address ranges.
#include "app/guest/FaultClass.h"

#include <doctest/doctest.h>

#if defined(_WIN32)
#include <cstring>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

TEST_CASE("a Windows access violation's first parameter says read, write or neither") {
    CHECK(AccessOfWindowsFault(0) == FaultAccess::kRead);
    CHECK(AccessOfWindowsFault(1) == FaultAccess::kWrite);
    CHECK(AccessOfWindowsFault(8) == FaultAccess::kUnknown);
    CHECK(AccessOfWindowsFault(2) == FaultAccess::kUnknown);
    CHECK(AccessViolationFlag(AccessOfWindowsFault(8)) == 0);
}

TEST_CASE("Windows exception codes are hardware faults where a POSIX host would signal") {
    CHECK(IsWindowsHardwareFault(0xC0000005u)); // access violation
    CHECK(IsWindowsHardwareFault(0xC0000006u)); // in-page error
    CHECK(IsWindowsHardwareFault(0x80000002u)); // misalignment
    CHECK(IsWindowsHardwareFault(0xC000001Du)); // illegal instruction
    CHECK(IsWindowsHardwareFault(0xC0000096u)); // privileged instruction
    CHECK(IsWindowsHardwareFault(0xC00000FDu)); // stack overflow
    CHECK(IsWindowsHardwareFault(0xC000008Cu)); // array bounds
    CHECK(IsWindowsHardwareFault(0xC000008Du)); // floating-point denormal operand
    CHECK(IsWindowsHardwareFault(0xC0000094u)); // integer divide by zero
    CHECK(IsWindowsHardwareFault(0xC0000095u)); // integer overflow
    CHECK_FALSE(IsWindowsHardwareFault(0xC000008Bu));
    CHECK_FALSE(IsWindowsHardwareFault(0xC0000097u));
    CHECK_FALSE(IsWindowsHardwareFault(0xE06D7363u)); // a C++ throw
    CHECK_FALSE(IsWindowsHardwareFault(0x80000003u)); // a breakpoint
    CHECK_FALSE(IsWindowsHardwareFault(0x40010006u)); // OutputDebugString
    CHECK_FALSE(IsWindowsHardwareFault(0x406D1388u)); // a thread's name, for a debugger
}

TEST_CASE("only a bad access or an in-page error names a memory address") {
    CHECK(IsWindowsMemoryFault(0xC0000005u));
    CHECK(IsWindowsMemoryFault(0xC0000006u));
    CHECK_FALSE(IsWindowsMemoryFault(0xC0000094u));
    CHECK_FALSE(IsWindowsMemoryFault(0xC00000FDu));
    CHECK_FALSE(IsWindowsMemoryFault(0x80000002u));
}

TEST_CASE("a routine's span runs to the next function, within the limit") {
    const CodeRange known = RoutineSpan(0x4000, 0x4340, 0x4000);
    CHECK(known.begin == 0x4000);
    CHECK(known.end == 0x4340);
    const CodeRange none = RoutineSpan(0x4000, 0, 0x4000);
    CHECK(none.end == 0x8000);
    const CodeRange distant = RoutineSpan(0x4000, 0x9000, 0x4000);
    CHECK(distant.end == 0x8000);
    const CodeRange behind = RoutineSpan(0x4000, 0x3000, 0x4000);
    CHECK(behind.end == 0x8000);
}

TEST_CASE("an Arm64 function's length comes from packed unwind data or its .xdata header") {
    // Packed (flag 1): bits 2-12 hold 0x23 instructions; the bits above them are other fields.
    CHECK(Arm64FunctionLength(0xABC00000u | (0x23u << 2) | 1u, 0xFFFFFFFFu) == 0x8Cu);
    // Packed (flag 2), the largest length the field holds.
    CHECK(Arm64FunctionLength((0x7FFu << 2) | 2u, 0u) == 0x1FFCu);
    // Flag 0: the word is the .xdata record's address; its header's bits 0-17 hold 0x1234.
    CHECK(Arm64FunctionLength(0x0001F000u, 0xFFFC0000u | 0x1234u) == 0x48D0u);
}

#if defined(_WIN32)
// The block-move routines the fault handler lets raise guest exceptions resolve to the routines
// themselves, each with an unwind entry that starts at its address, not to a thunk that jumps
// there; their spans end where that entry does, so no other host code lies inside one.
TEST_CASE("the C runtime's block-move routines span exactly their own unwind entries") {
    for (const void *routine : {reinterpret_cast<const void *>(&std::memmove),
                                reinterpret_cast<const void *>(&std::memcpy),
                                reinterpret_cast<const void *>(&std::memset),
                                reinterpret_cast<const void *>(&std::memcmp)}) {
        const auto address = reinterpret_cast<std::uintptr_t>(routine);
        DWORD64 imageBase = 0;
        const RUNTIME_FUNCTION *entry =
            RtlLookupFunctionEntry(static_cast<DWORD64>(address), &imageBase, nullptr);
        REQUIRE(entry != nullptr);
        CHECK(imageBase + entry->BeginAddress == address);
        const CodeRange span = MemoryRoutineSpan(routine);
        CHECK(span.begin == address);
        CHECK(span.end > span.begin);
        CHECK(span.end - span.begin < 0x2000u);
#if !defined(_M_ARM64) && !defined(__aarch64__)
        CHECK(span.end == imageBase + entry->EndAddress);
#endif
    }
}
#endif

} // namespace sfcop::guest
