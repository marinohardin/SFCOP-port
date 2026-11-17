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
// first read fault in one of its host pages fills that page with environment-shaped text, makes
// it read-only, records who read it, and resumes the instruction, which then reads the text. The
// fault handler does only that (system calls, a fill and atomics); the record is turned into a
// log line naming the code that read it later, outside the handler (TakeReads).
//
// A write to the region still faults, as does any read of it with the switch off, so those keep
// ending the run as before. The oracle and CI are to run with the switch off, so that such reads
// show up there.
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
// so a 32-bit read at any address in the region, aligned or not, is at least 0x00200020 and at
// most 0x7E007E00: never 0 and never a small id such as a team or an index. It can be the address
// of something (0x00400000 and up is the game's image), as two characters of a real environment
// block can; code that took it for a pointer reads whatever is there, as on the original.
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

    // `guestZero` is the host address of guest 0. A host page larger than the region, one that
    // does not divide it, or a guest 0x00010000 that does not start a host page (Aligned) leaves
    // the region disabled.
    LowMemoryRegion(std::uint8_t *guestZero, std::size_t hostPage);

    // Makes the region inaccessible and enables the region. The region's memory must already be
    // mapped (committed), readable and writable. False, and the region stays disabled, when the
    // host refuses, the page size does not fit or the region does not start a host page.
    bool Arm();
    bool Armed() const { return armed_; }

    // Whether guest 0x00010000 starts a host page; the region protects whole host pages only.
    bool Aligned() const { return aligned_; }

    // A read the region answered: the guest address read, the guest function current then
    // (g_cur_func) and the host instruction that read it.
    struct Read {
        std::uint32_t address = 0;
        std::uint32_t function = 0;
        std::uintptr_t pc = 0;
    };

    // A fault at guest address `guest` with `access`, by guest function `function` at host
    // instruction `pc`. Safe in a signal handler: system calls, the page's fill and atomics only;
    // nothing is formatted or printed here. A page this call maps records the read for TakeReads.
    //
    // A read, or an access the platform cannot tell (read faults are the case this serves; a
    // write to a page this has already made readable is passed on, so it cannot loop), in an
    // inaccessible page is mapped; everything else is not handled.
    Outcome OnFault(std::uint32_t guest, FaultAccess access, std::uint32_t function = 0,
                    std::uintptr_t pc = 0);

    // The reads recorded since the last call, each once, in page order, up to `max` of them into
    // `out`; how many. Not for a signal handler's use: its caller formats and prints them.
    std::size_t TakeReads(Read *out, std::size_t max);

    // How long a fault waits for another to finish mapping its page before it gives up and ends
    // the run (one mapping takes microseconds; a wait this long means it cannot finish).
    static constexpr std::uint32_t kMappingWaitLimit = 1u << 26;

    std::size_t PageCount() const { return pages_; }

private:
    static constexpr std::size_t kMaxPages = 16; // 4 KB pages; 16 KB pages give 4, 64 KB pages 1
    enum : std::uint8_t { kNone, kMapping, kReadable };

    std::uint8_t *guestZero_;
    std::size_t hostPage_;
    std::size_t pages_ = 0;
    bool aligned_ = false;
    bool armed_ = false;
    std::atomic<std::uint8_t> state_[kMaxPages] = {};
    Read reads_[kMaxPages] = {};                 // at most one per page, written before it is published
    std::atomic<bool> reported_[kMaxPages] = {}; // its read handed out by TakeReads
};

// How SetUpLowMemory left the region.
enum class LowMemorySetUp {
    kOff,           // the switch is off: nothing reserved, nothing armed
    kArmed,         // reserved and armed
    kMisaligned,    // guest 0x00010000 does not start a host page: nothing reserved; off
    kInUse,         // the range could not be reserved: off
    kCannotProtect, // reserved, but the host refused to protect it: released again; off
};

// The guest host's set-up of the region for fix_hle_low_memory_reads at boot. With `on`, reserves
// guest 0x00010000-0x0001FFFF with `reserve` (the guest memory's own, which maps the range
// committed, readable and writable on every host) so that nothing else is placed there, then
// arms `region` over it; `release` gives it back when arming fails. With `on` false it calls
// neither and arms nothing, so a read of the range faults as without the fix.
LowMemorySetUp SetUpLowMemory(bool on, LowMemoryRegion &region,
                              std::uint32_t (*reserve)(std::uint32_t va, std::uint32_t size),
                              void (*release)(std::uint32_t va, std::uint32_t size));

} // namespace sfcop::guest
