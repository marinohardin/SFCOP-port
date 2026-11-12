#include "hle/kernel32rt/CriticalSections.h"

#include <cstring>
#include <iterator>

namespace sfcop::hle::kernel32rt {

namespace {

std::uint32_t load(const GuestSection &section, std::uint32_t offset) {
    std::uint32_t value;
    std::memcpy(&value, section.bytes + offset, sizeof value);
    return value;
}

void store(const GuestSection &section, std::uint32_t offset, std::uint32_t value) {
    std::memcpy(section.bytes + offset, &value, sizeof value);
}

constexpr std::uint32_t kFree = 0xFFFFFFFFu; // LockCount of a free section (-1)

// The section's LockCount, with a never-initialised section (all zero: no initialised section is
// free with a LockCount of 0) made free first.
std::uint32_t lockCountOf(const GuestSection &section, std::uint32_t owner) {
    std::uint32_t count = load(section, kSectionLockCount);
    if (owner == 0 && count == 0) {
        std::memset(section.bytes, 0, kSectionBytes);
        count = kFree;
    }
    return count;
}

} // namespace

void CriticalSections::Initialize(GuestSection section) {
    reset(section);
}

bool CriticalSections::TryEnter(GuestSection section, std::uint32_t thread) {
    const std::uint32_t owner = load(section, kSectionOwningThread);
    const std::uint32_t count = lockCountOf(section, owner);
    if (owner == 0) {
        store(section, kSectionLockCount, count + 1);
        store(section, kSectionRecursionCount, 1);
        store(section, kSectionOwningThread, thread);
        ownedBy(thread).push_back(section);
        return true;
    }
    if (owner == thread) {
        store(section, kSectionLockCount, count + 1);
        store(section, kSectionRecursionCount, load(section, kSectionRecursionCount) + 1);
        return true;
    }
    return false;
}

void CriticalSections::Enter(GuestSection section, std::uint32_t thread, Machine &machine) {
    if (TryEnter(section, thread)) {
        return;
    }
    // Owned by another thread: count this thread in LockCount, queue it, and sleep without the
    // machine until the owner's last Leave hands the section over.
    store(section, kSectionLockCount, load(section, kSectionLockCount) + 1);
    Waiter me{thread, false};
    std::shared_ptr<WaitObject> object = waitObjectFor(section.address);
    {
        std::lock_guard<std::mutex> hold(lock_);
        object->queue.push_back(&me);
    }
    machine.GiveUp();
    {
        std::unique_lock<std::mutex> hold(lock_);
        granted_.wait(hold, [&me] { return me.granted; });
    }
    machine.TakeBack();
}

bool CriticalSections::Leave(GuestSection section, std::uint32_t thread) {
    if (load(section, kSectionOwningThread) != thread || thread == 0) {
        return false;
    }
    const std::uint32_t count = load(section, kSectionLockCount) - 1;
    const std::uint32_t recursion = load(section, kSectionRecursionCount) - 1;
    store(section, kSectionLockCount, count);
    store(section, kSectionRecursionCount, recursion);
    if (recursion != 0) {
        return true;
    }
    store(section, kSectionOwningThread, 0);
    noteReleased(thread, section.address);
    if (count != kFree) { // someone is counted waiting
        handOn(section);
    }
    return true;
}

void CriticalSections::Delete(GuestSection section) {
    reset(section);
}

// Initialize and Delete: the structure becomes a free section and its wait object is dropped. A
// thread still queued on it (the game deleted or re-initialised a section another thread waits for,
// which Windows leaves undefined) is not left asleep on a wait object nobody will signal: the first
// is handed the fresh section, and the rest stay queued and counted behind it.
void CriticalSections::reset(GuestSection section) {
    const std::uint32_t owner = load(section, kSectionOwningThread);
    if (owner != 0) { // bytes never initialised name no thread that has a list, and are skipped
        noteReleased(owner, section.address);
    }
    std::memset(section.bytes, 0, kSectionBytes);
    store(section, kSectionLockCount, kFree);
    std::lock_guard<std::mutex> hold(lock_);
    const auto found = waiting_.find(section.address);
    if (found == waiting_.end()) {
        return;
    }
    auto &queue = found->second->queue;
    if (queue.empty()) {
        waiting_.erase(found);
        return;
    }
    Waiter *next = queue.front();
    queue.pop_front();
    store(section, kSectionLockCount, static_cast<std::uint32_t>(queue.size()));
    grant(section, next->thread);
    next->granted = true;
    granted_.notify_all();
    if (queue.empty()) {
        waiting_.erase(found);
    }
}

std::size_t CriticalSections::WaitObjects() {
    std::lock_guard<std::mutex> hold(lock_);
    return waiting_.size();
}

std::shared_ptr<CriticalSections::WaitObject> CriticalSections::waitObjectFor(std::uint32_t address) {
    std::lock_guard<std::mutex> hold(lock_);
    auto &slot = waiting_[address];
    if (!slot) {
        slot = std::make_shared<WaitObject>();
    }
    return slot;
}

// The section has just been left with a waiter counted: it becomes the first queued thread's.
void CriticalSections::handOn(GuestSection section) {
    std::lock_guard<std::mutex> hold(lock_);
    const auto found = waiting_.find(section.address);
    if (found == waiting_.end() || found->second->queue.empty()) {
        // A count with no queued thread (the game wrote the structure itself): leave it free.
        store(section, kSectionLockCount, kFree);
        return;
    }
    Waiter *next = found->second->queue.front();
    found->second->queue.pop_front();
    grant(section, next->thread);
    next->granted = true;
    granted_.notify_all();
}

// The section becomes `thread`'s, entered once (LockCount is the caller's to set).
void CriticalSections::grant(GuestSection section, std::uint32_t thread) {
    store(section, kSectionRecursionCount, 1);
    store(section, kSectionOwningThread, thread);
    ownedBy(thread).push_back(section);
}

std::vector<std::uint32_t> CriticalSections::ThreadEnded(std::uint32_t thread) {
    std::vector<GuestSection> held;
    held.swap(ownedBy(thread));
    owned_.erase(thread);
    cachedThread_ = 0;
    cachedOwned_ = nullptr;
    std::vector<std::uint32_t> released;
    for (auto it = held.rbegin(); it != held.rend(); ++it) {
        const GuestSection section = *it;
        if (load(section, kSectionOwningThread) != thread) {
            continue; // already given up some other way
        }
        const std::uint32_t count =
            load(section, kSectionLockCount) - load(section, kSectionRecursionCount);
        store(section, kSectionLockCount, count);
        store(section, kSectionRecursionCount, 0);
        store(section, kSectionOwningThread, 0);
        released.push_back(section.address);
        if (count != kFree) {
            handOn(section);
        }
    }
    return released;
}

std::vector<GuestSection> &CriticalSections::ownedBy(std::uint32_t thread) {
    if (cachedOwned_ == nullptr || cachedThread_ != thread) {
        cachedOwned_ = &owned_[thread]; // unordered_map elements keep their address on rehash
        cachedThread_ = thread;
    }
    return *cachedOwned_;
}

// `thread` no longer owns the section at `address`: off its list (usually the last one taken).
void CriticalSections::noteReleased(std::uint32_t thread, std::uint32_t address) {
    std::vector<GuestSection> *list = cachedThread_ == thread ? cachedOwned_ : nullptr;
    if (list == nullptr) {
        const auto found = owned_.find(thread);
        if (found == owned_.end()) {
            return;
        }
        list = &found->second;
    }
    std::vector<GuestSection> &held = *list;
    for (auto it = held.rbegin(); it != held.rend(); ++it) {
        if (it->address == address) {
            held.erase(std::next(it).base());
            return;
        }
    }
}

} // namespace sfcop::hle::kernel32rt
