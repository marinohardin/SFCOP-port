#include "hle/kernel32rt/CriticalSections.h"

namespace sfcop::hle::kernel32rt {

CriticalSections::CriticalSections(platform::Threads &threads) : threads_(threads) {}

void CriticalSections::Initialize(std::uint32_t address) {
    auto made = threads_.MakeMutex(false, {});
    std::lock_guard<std::mutex> hold(lock_);
    auto &slot = sections_[address];
    if (slot.Valid()) {
        static_cast<void>(threads_.Close(slot)); // our own handle: nothing to report
    }
    slot = made.Ok() ? made.Value().handle : platform::Waitable{};
}

platform::Waitable CriticalSections::mutexFor(std::uint32_t address) {
    std::lock_guard<std::mutex> hold(lock_);
    auto &slot = sections_[address];
    if (!slot.Valid()) {
        auto made = threads_.MakeMutex(false, {});
        if (made.Ok()) {
            slot = made.Value().handle;
        }
    }
    return slot;
}

bool CriticalSections::TryEnter(std::uint32_t address) {
    const platform::Waitable mutex = mutexFor(address);
    const auto waited = threads_.Wait(&mutex, 1, platform::WaitMode::kAny, 0u);
    return waited.Ok() && waited.Value().outcome != platform::WaitOutcome::kTimedOut;
}

void CriticalSections::Enter(std::uint32_t address) {
    const platform::Waitable mutex = mutexFor(address);
    // A wait without a timeout ends with the mutex taken, or abandoned and taken.
    static_cast<void>(threads_.Wait(&mutex, 1, platform::WaitMode::kAny, std::nullopt));
}

bool CriticalSections::Leave(std::uint32_t address) {
    const platform::Waitable mutex = mutexFor(address);
    return threads_.ReleaseMutex(mutex).Ok();
}

void CriticalSections::Delete(std::uint32_t address) {
    std::lock_guard<std::mutex> hold(lock_);
    const auto found = sections_.find(address);
    if (found != sections_.end()) {
        if (found->second.Valid()) {
            static_cast<void>(threads_.Close(found->second));
        }
        sections_.erase(found);
    }
}

} // namespace sfcop::hle::kernel32rt
