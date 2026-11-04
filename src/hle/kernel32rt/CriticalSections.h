// CriticalSections: the game's CRITICAL_SECTIONs (InitializeCriticalSection, Enter, Leave, Delete),
// each a recursive Threads mutex found by the structure's guest address.
//
// The 24 bytes of the guest structure are left as InitializeCriticalSection wrote them; nothing in
// the game or its C runtime reads the owner or the counts. A section the game enters without
// initialising (a zeroed static) gets its mutex on first use.
//
// Enter() blocks; the shim first tries TryEnter() and gives the guest machine up only around an
// Enter() that has to wait. Threading: any thread; the address table is behind a lock.
#pragma once

#include "platform/Threads/Threads.h"

#include <cstdint>
#include <map>
#include <mutex>

namespace sfcop::hle::kernel32rt {

class CriticalSections {
public:
    explicit CriticalSections(platform::Threads &threads);

    // InitializeCriticalSection: a new, free mutex for `address` (an earlier one is closed).
    void Initialize(std::uint32_t address);
    // Takes the section if it is free or already the caller's; false if another thread has it.
    bool TryEnter(std::uint32_t address);
    // EnterCriticalSection: waits until the section is the caller's.
    void Enter(std::uint32_t address);
    // LeaveCriticalSection: false if the caller does not hold it (Windows does not report this;
    // the shim logs it once).
    bool Leave(std::uint32_t address);
    // DeleteCriticalSection: forgets it; a later Enter makes a new one.
    void Delete(std::uint32_t address);

private:
    platform::Waitable mutexFor(std::uint32_t address);

    platform::Threads &threads_;
    std::mutex lock_;
    std::map<std::uint32_t, platform::Waitable> sections_;
};

} // namespace sfcop::hle::kernel32rt
