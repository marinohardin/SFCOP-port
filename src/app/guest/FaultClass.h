// FaultClass: which hardware faults become guest exceptions (GuestExceptions.cpp).
//
// A guest exception is delivered by a longjmp to the landing pad of the lifted function that
// catches it, over every frame in between. That is sound only when every skipped frame is lifted
// code (plain C, no destructors, no locks held). A fault in host code (an HLE module, the
// standard library) is a defect of the host, not a guest event: it is reported and ends the run.
//
// Known limit: the lifted code's range is the span of the dispatch table's function addresses
// plus a margin for the last function; the linker places the lift's objects together, but a
// host function that lands inside the span would be taken for lifted code.
//
// No lifted code in this file: it is built into the tests too.
#pragma once

#include <cstdint>
#include <vector>

namespace sfcop::guest {

// A half-open span of host code addresses [begin, end).
struct CodeRange {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
};

class CodeRanges {
public:
    void Add(CodeRange range) { ranges_.push_back(range); }
    bool Contains(std::uintptr_t address) const {
        for (const CodeRange &range : ranges_) {
            if (address >= range.begin && address < range.end) {
                return true;
            }
        }
        return false;
    }

private:
    std::vector<CodeRange> ranges_;
};

// The span of `count` function addresses, `margin` bytes past the highest (the size of the last
// function is not known). An empty list is an empty span.
CodeRange SpanOfFunctions(const std::uintptr_t *addresses, std::size_t count, std::uintptr_t margin);

// Whether `symbol` names a leaf memory routine of the C library (memmove, memcpy, memset, memcmp,
// bzero, in any platform's spelling: `_platform_memmove`, `__memcpy_avx_unaligned`). The lifted
// code's string and block moves call these directly, so a fault inside one is a fault of the
// guest's access, and no host frame lies between it and the lifted caller. nullptr: no.
bool IsLeafMemoryRoutine(const char *symbol);

// How a faulting access touched memory, where the platform's fault context says.
enum class FaultAccess { kRead, kWrite, kUnknown };

// What a Windows host's access violation says it was (EXCEPTION_RECORD's ExceptionInformation[0]):
// 0 a read, 1 a write, 8 an execute (data execution prevention). An execute, or a value Windows
// does not document, is of unknown kind.
constexpr FaultAccess AccessOfWindowsFault(std::uint64_t operation) {
    return operation == 0 ? FaultAccess::kRead
           : operation == 1 ? FaultAccess::kWrite
                            : FaultAccess::kUnknown;
}

// What a Windows host's exception code is, as the signal a POSIX host would deliver for it: a
// hardware fault the guest host handles (a bad access, an illegal or privileged instruction, an
// integer or floating-point fault, a misaligned access, a stack overflow), and among those, the
// accesses to memory that may be the guest's (EXCEPTION_ACCESS_VIOLATION and
// EXCEPTION_IN_PAGE_ERROR, whose second parameter is the address). Every other code (a C++
// throw, a debugger's notice, a breakpoint) is not a fault and is left to Windows.
bool IsWindowsHardwareFault(std::uint32_t code);
bool IsWindowsMemoryFault(std::uint32_t code);

// The span of a routine that starts at `begin` when only where the next function starts is known
// (`next`, 0 when none was found): up to it, and never more than `limit` bytes.
constexpr CodeRange RoutineSpan(std::uintptr_t begin, std::uintptr_t next, std::uintptr_t limit) {
    return CodeRange{begin, next > begin && next - begin < limit ? next : begin + limit};
}

// An Arm64 Windows function's length in bytes, from its unwind table entry's second word: packed
// unwind data (low two bits nonzero) holds it in bits 2-12; otherwise that word locates an .xdata
// record whose first word (`xdataHeader`, read only then) holds it in bits 0-17. Both count
// 4-byte instructions.
constexpr std::uint32_t Arm64FunctionLength(std::uint32_t unwindData, std::uint32_t xdataHeader) {
    return (unwindData & 3u) != 0 ? ((unwindData >> 2) & 0x7FFu) * 4u : (xdataHeader & 0x3FFFFu) * 4u;
}

#if defined(_WIN32)
// The span of one of the C runtime's block-move routines (memmove...) in this image, which the
// static C runtime puts in the client itself: the routine's own unwind table entry, when it has
// one that starts at the routine; otherwise up to the next function the table lists, and never
// more than kNoEntryLimit bytes, so that a span cannot reach into other host code.
constexpr std::uintptr_t kNoEntryLimit = 256;
CodeRange MemoryRoutineSpan(const void *routine);
#endif

// EXCEPTION_ACCESS_VIOLATION's first parameter: 1 for a write, else 0. An access of unknown kind
// is recorded as a read.
constexpr std::uint32_t AccessViolationFlag(FaultAccess access) {
    return access == FaultAccess::kWrite ? 1u : 0u;
}

} // namespace sfcop::guest
