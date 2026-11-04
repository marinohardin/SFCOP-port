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

// EXCEPTION_ACCESS_VIOLATION's first parameter: 1 for a write, else 0. An access of unknown kind
// is recorded as a read.
constexpr std::uint32_t AccessViolationFlag(FaultAccess access) {
    return access == FaultAccess::kWrite ? 1u : 0u;
}

} // namespace sfcop::guest
