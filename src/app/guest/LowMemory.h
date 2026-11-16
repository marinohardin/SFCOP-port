// LowMemory: the guest's 64 KB at 0x00010000, readable on first touch (fix_hle_low_memory_reads).
//
// Code that reads a field through a null pointer to a large object reads guest 0 + the field's
// offset. Below 64 KB that faults on Windows too. Between 0x00010000 and 0x0001FFFF it did not on
// the original's Windows NT target: there, process creation places the new process's environment
// block at 0x00010000 (documented NT behaviour), so such a read returns two UTF-16 characters of
// it and the code goes on with a value that is not a small id or a valid pointer. The original
// game and its mission scripts have reads like that (a mission's victory state reads a team that
// the mission never creates); the port leaves the region unmapped, so they end the run.
//
// With the switch on, the guest host reserves the region at boot and leaves it inaccessible. The
// first read fault in one of its host pages logs one line naming the code that read it, fills
// that page with environment-shaped text, makes it read-only, and resumes the instruction, which
// then reads the text. A write still faults, as does any read with the switch off, so those keep
// ending the run as before; the oracle and CI run with the switch off so that such reads show up
// there.
//
// The region is described by the host address of guest 0 and the host page size, so the tests
// can stand in a block of their own for the guest's memory.
#pragma once

#include "app/guest/FaultClass.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace sfcop::guest {

inline constexpr std::uint32_t kLowMemoryBegin = 0x00010000u;
inline constexpr std::uint32_t kLowMemoryEnd = 0x00020000u; // excluded

inline constexpr bool InLowMemory(std::uint32_t guest) {
    return guest >= kLowMemoryBegin && guest < kLowMemoryEnd;
}

// The text the region reads as, one UTF-16 code unit per character, repeated over the region from
// its first byte. Every character is printable ASCII (0x20 to 0x7E) and there is no NUL anywhere,
// so a 32-bit read at any address in the region, aligned or not, is at least 0x00200020: never 0,
// never a small id such as a team or an index, and never a pointer into the guest's low memory.
// A real environment block separates its entries with NULs; here a space stands in for each, for
// that reason. The entries are invented and name no real machine, user or folder; the text is
// about as long as a typical environment of the time.
const char *LowMemoryText();
std::size_t LowMemoryTextLength();

// Writes the region's contents for [guest, guest + bytes) to `out`. `guest` and `bytes` are even
// and the range lies in the region.
void FillLowMemory(std::uint8_t *out, std::uint32_t guest, std::size_t bytes);

// The report line for a read at `address` by `reader` ("STARFLEETOP 0x00412345",
// "Met_17Patrol+0xEBB0"), written to `out` with its newline; the line's length.
std::size_t FormatLowMemoryRead(char *out, std::size_t size, const char *reader, std::uint32_t address);

// The name of a module file without its folders and extension: "a/b/Met_17Patrol.dylib" gives
// "Met_17Patrol". Writes to `out` (always terminated) and returns it.
const char *ModuleStem(const char *path, char *out, std::size_t size);

class LowMemoryRegion {
public:
    enum class Outcome {
        kNotHandled, // not this region's fault: the caller handles it as before
        kMapped,     // this call made the page readable and should log the read; resume
        kRetry,      // another fault made the page readable; resume
    };

    // `guestZero` is the host address of guest 0. A host page larger than the region, or one that
    // does not divide it, leaves the region disabled.
    LowMemoryRegion(std::uint8_t *guestZero, std::size_t hostPage);

    // Makes the region inaccessible and enables the region. The region's memory must already be
    // mapped (committed), readable and writable. False, and the region stays disabled, when the
    // host refuses or the page size does not fit.
    bool Arm();
    bool Armed() const { return armed_; }

    // A fault at guest address `guest` with `access`. Safe in a signal handler: system calls and
    // atomics only. A read, or an access the platform cannot tell (read faults are the case this
    // serves; a write to a page this has already made readable is passed on, so it cannot loop),
    // in an inaccessible page is mapped; everything else is not handled.
    Outcome OnFault(std::uint32_t guest, FaultAccess access);

    std::size_t PageCount() const { return pages_; }

private:
    static constexpr std::size_t kMaxPages = 16; // 4 KB pages; 16 KB pages give 4, 64 KB pages 1
    enum : std::uint8_t { kNone, kMapping, kReadable };

    std::uint8_t *guestZero_;
    std::size_t hostPage_;
    std::size_t pages_ = 0;
    bool armed_ = false;
    std::atomic<std::uint8_t> state_[kMaxPages] = {};
};

} // namespace sfcop::guest
