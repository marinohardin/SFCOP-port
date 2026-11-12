// CriticalSections: the game's CRITICAL_SECTIONs (InitializeCriticalSection, Enter, TryEnter, Leave,
// Delete), kept in the guest's own 24-byte structure as Windows XP keeps it:
//
//   +0  DebugInfo       0 (no debug record)
//   +4  LockCount       -1 when free; +1 for every Enter held (recursion included) or waiting
//   +8  RecursionCount  how many times the owner has entered
//   +12 OwningThread    the owner's thread id (GetCurrentThreadId's), 0 when free
//   +16 LockSemaphore   0 (the wait object is the host's, below)
//   +20 SpinCount       0
//
// Every call runs with the guest machine held, and one thread holds it at a time, so the structure
// is read and written without a host lock: an uncontended Enter or Leave touches only those bytes.
// A thread that finds the section owned by another queues itself on a host object kept for that
// section (made on the first contention, released by DeleteCriticalSection), gives the machine up
// and sleeps; the owner's last Leave hands the section to the first queued thread (OwningThread is
// already the waiter's when it wakes) and wakes it. A free section therefore never has a waiter.
//
// A section entered without InitializeCriticalSection (all 24 bytes zero, which no initialised
// section is) is initialised on first use.
//
// A thread that ends while it owns sections releases them (ThreadEnded), and each one's first
// waiter is handed it. Windows leaves such a section owned for ever. This is a deliberate departure,
// kept from the mutex-backed sections this replaces: a guest thread here ends through ExitThread's
// unwind as well as by returning, and a game thread that ended inside a section must not hang the
// next thread to enter it. So every section's owner keeps a list of the sections it holds, which
// the uncontended path keeps with one push and one pop (no lock: it is guarded by the machine).
//
// Threading: any thread, but only one holding the guest machine; the waiter table is behind a lock.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace sfcop::hle::kernel32rt {

// A guest CRITICAL_SECTION: its guest address (the key of its host wait object) and its 24 bytes.
struct GuestSection {
    std::uint32_t address = 0;
    std::uint8_t *bytes = nullptr;
};

// Offsets of the structure's fields, and its size.
constexpr std::uint32_t kSectionDebugInfo = 0;
constexpr std::uint32_t kSectionLockCount = 4;
constexpr std::uint32_t kSectionRecursionCount = 8;
constexpr std::uint32_t kSectionOwningThread = 12;
constexpr std::uint32_t kSectionLockSemaphore = 16;
constexpr std::uint32_t kSectionSpinCount = 20;
constexpr std::uint32_t kSectionBytes = 24;

// The guest machine, given up around a wait for another thread's section and taken back after.
class Machine {
public:
    virtual ~Machine() = default;
    virtual void GiveUp() = 0;
    virtual void TakeBack() = 0;
};

class CriticalSections {
public:
    // InitializeCriticalSection: a free section; an earlier wait object at the address is dropped.
    // A thread still waiting on it is handed the new section instead of being left asleep.
    void Initialize(GuestSection section);
    // TryEnterCriticalSection: takes the section if it is free or already `thread`'s; false (and
    // nothing changed) if another thread has it.
    bool TryEnter(GuestSection section, std::uint32_t thread);
    // EnterCriticalSection: returns with the section `thread`'s, giving `machine` up while it waits.
    void Enter(GuestSection section, std::uint32_t thread, Machine &machine);
    // LeaveCriticalSection: false, and nothing changed, if `thread` does not hold it (Windows does
    // not report this).
    bool Leave(GuestSection section, std::uint32_t thread);
    // DeleteCriticalSection: drops the wait object and leaves the structure as a free section with
    // no debug record; a later Enter uses it again. A thread still waiting on it is handed the
    // section instead of being left asleep.
    void Delete(GuestSection section);
    // `thread` has ended: every section it still owns is released, whatever its recursion, and
    // handed to its first waiter. Returns the guest addresses released, in the order released
    // (the most recently taken first). Called with the machine held.
    std::vector<std::uint32_t> ThreadEnded(std::uint32_t thread);

    // How many sections have a host wait object (tests).
    std::size_t WaitObjects();

private:
    // One thread queued on a section: set `granted` when the section has been handed to it.
    struct Waiter {
        std::uint32_t thread = 0;
        bool granted = false;
    };
    // A section's host wait object: its queue, in arrival order.
    struct WaitObject {
        std::deque<Waiter *> queue;
    };

    std::shared_ptr<WaitObject> waitObjectFor(std::uint32_t address);
    void handOn(GuestSection section);
    void reset(GuestSection section);
    void grant(GuestSection section, std::uint32_t thread);
    std::vector<GuestSection> &ownedBy(std::uint32_t thread);
    void noteReleased(std::uint32_t thread, std::uint32_t address);

    std::mutex lock_; // guards waiting_ and every WaitObject's queue and grants
    std::condition_variable granted_;
    std::map<std::uint32_t, std::shared_ptr<WaitObject>> waiting_;

    // The sections each thread owns, for ThreadEnded; guarded by the machine, not by lock_. The
    // last thread looked up is cached, so the uncontended path does not hash.
    std::unordered_map<std::uint32_t, std::vector<GuestSection>> owned_;
    std::uint32_t cachedThread_ = 0;
    std::vector<GuestSection> *cachedOwned_ = nullptr;
};

} // namespace sfcop::hle::kernel32rt
