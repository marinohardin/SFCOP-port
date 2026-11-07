#include "platform/Clock/FixedStepClock.h"

#include <utility>

namespace sfcop::platform {

FixedStepClock::FixedStepClock(std::unique_ptr<Clock> inner, std::uint64_t quantumUnits)
    : inner_(std::move(inner)), quantum_(quantumUnits == 0 ? 1 : quantumUnits) {}

void FixedStepClock::BindToCallingThread() {
    frameThread_ = std::this_thread::get_id();
    bound_.store(true);
}

bool FixedStepClock::OnFrameThread() const {
    return bound_.load() && std::this_thread::get_id() == frameThread_;
}

void FixedStepClock::Grow(std::uint64_t units) {
    units_.fetch_add(units);
    if (OnFrameThread()) {
        readsSinceGrowth_ = 0;
    }
}

void FixedStepClock::AdvanceFrame() {
    frames_.fetch_add(1);
    Grow(quantum_);
}

std::uint64_t FixedStepClock::Read() {
    if (OnFrameThread() && ++readsSinceGrowth_ >= kSpinReads) {
        spinSteps_.fetch_add(1);
        Grow(kUnitsPerMs);
    }
    return units_.load();
}

Result<std::uint32_t> FixedStepClock::Ticks() {
    // Truncated to 32 bits: the counter wraps as the game's tick counters do.
    return static_cast<std::uint32_t>(Read() / kUnitsPerMs);
}

Result<std::uint64_t> FixedStepClock::Counter() { return Read(); }

Result<std::uint64_t> FixedStepClock::CounterFrequency() { return kFrequency; }

Result<CivilTime> FixedStepClock::Now(TimeZone zone) {
    if (!inner_) {
        return Error::kUnsupported;
    }
    return inner_->Now(zone);
}

Result<std::int64_t> FixedStepClock::UnixTime() {
    if (!inner_) {
        return Error::kUnsupported;
    }
    return inner_->UnixTime();
}

Status FixedStepClock::Sleep(std::uint32_t milliseconds) {
    if (milliseconds != 0 && OnFrameThread()) {
        Grow(static_cast<std::uint64_t>(milliseconds) * kUnitsPerMs);
    }
    if (!inner_) {
        return {};
    }
    return inner_->Sleep(milliseconds);
}

} // namespace sfcop::platform
